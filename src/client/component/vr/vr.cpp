#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "vr.hpp"
#include "vr_log.hpp"
#include "xr_runtime.hpp"

#include "component/command.hpp"
#include "component/directx.hpp"
#include "component/console/console.hpp"

#include "game/game.hpp"

#include "version.hpp"

#include <utils/flags.hpp>

// Milestone 1: the "virtual screen".
// The finished flat image the game is about to present is copied every frame onto a big screen floating in
// front of the player, rendered by the headset's OpenXR runtime. It proves the whole pipeline (runtime, session,
// swapchains, frame pacing) inside the game process. Head tracking of the game camera and stereo come next.

namespace vr
{
	namespace
	{
		using vr_clock = std::chrono::steady_clock;

		constexpr auto runtime_retry_delay = 10s;
		constexpr auto headset_retry_delay = 3s;

		// guards `runtime` between the render thread (frame loop) and console commands
		std::timed_mutex runtime_mutex;
		std::unique_ptr<xr_runtime> runtime;
		vr_clock::time_point next_attempt{};
		bool device_logged = false;

		std::atomic_bool vr_disabled = false;
		std::atomic_bool restart_requested = false;
		std::atomic_bool recenter_requested = false;

		game::dvar_t* vr_screen_distance = nullptr;
		game::dvar_t* vr_screen_width = nullptr;
		game::dvar_t* vr_screen_height = nullptr;
		game::dvar_t* vr_screen_follow_head = nullptr;
		game::dvar_t* vr_screen_srgb = nullptr;

		// the game guards its D3D11 immediate context with a Win32 mutex; we take it for anything that may touch the context
		class d3d_lock_guard
		{
		public:
			explicit d3d_lock_guard(const HANDLE lock)
				: lock_(lock)
			{
				if (this->lock_)
				{
					WaitForSingleObject(this->lock_, INFINITE);
				}
			}

			~d3d_lock_guard()
			{
				if (this->lock_)
				{
					ReleaseMutex(this->lock_);
				}
			}

			d3d_lock_guard(const d3d_lock_guard&) = delete;
			d3d_lock_guard& operator=(const d3d_lock_guard&) = delete;

		private:
			HANDLE lock_;
		};

		screen_params read_screen_params()
		{
			screen_params params{};

			if (vr_screen_distance)
			{
				params.distance = vr_screen_distance->current.value;
			}

			if (vr_screen_width)
			{
				params.width = vr_screen_width->current.value;
			}

			if (vr_screen_height)
			{
				params.height_offset = vr_screen_height->current.value;
			}

			if (vr_screen_follow_head)
			{
				params.follow_head = vr_screen_follow_head->current.enabled;
			}

			if (vr_screen_srgb)
			{
				params.srgb = vr_screen_srgb->current.enabled;
			}

			return params;
		}

		void log_device_once(IDXGISwapChain* swap_chain)
		{
			if (device_logged)
			{
				return;
			}

			device_logged = true;

			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(swap_chain->GetDesc(&desc)))
			{
				log::info("Game swap chain: {}x{}, format {}, {} buffers, {} samples, swap effect {}, windowed {}",
					desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
					desc.SampleDesc.Count, static_cast<int>(desc.SwapEffect), desc.Windowed ? "yes" : "no");
			}

			log::info("Game D3D11 feature level: {:#x}", static_cast<unsigned>(dx::device->GetFeatureLevel()));
		}

		// Brings up the OpenXR instance, then the session. Returns true once a session exists.
		bool ensure_runtime()
		{
			const auto now = vr_clock::now();
			if (now < next_attempt)
			{
				return false;
			}

			try
			{
				if (!runtime)
				{
					runtime = std::make_unique<xr_runtime>();
				}

				if (!runtime->try_create_session(dx::device))
				{
					next_attempt = now + headset_retry_delay;
					return false;
				}

				return true;
			}
			catch (const xr_error& e)
			{
				runtime.reset();

				if (e.retry())
				{
					log::warn("{} Trying again in {} seconds.", e.what(), runtime_retry_delay.count());
					next_attempt = now + runtime_retry_delay;
				}
				else
				{
					log::error("{}", e.what());
					log::error("VR is off. Fix the problem above, then type vr_restart in the console (~ key).");
					vr_disabled = true;
				}

				return false;
			}
		}

		void print_status()
		{
			std::unique_lock lock(runtime_mutex, std::defer_lock);
			if (!lock.try_lock_for(500ms))
			{
				console::info("[VR] busy, try again\n");
				return;
			}

			if (runtime)
			{
				console::info("[VR] %s\n", runtime->status().data());
			}
			else
			{
				console::info("[VR] %s\n", vr_disabled ? "off (see iw7-vr.log in the game folder)" : "waiting for the headset runtime");
			}
		}
	}

	void on_present(IDXGISwapChain* swap_chain, const HANDLE d3d_lock)
	{
		if (vr_disabled || !swap_chain || !dx::device || !dx::deviceContext)
		{
			return;
		}

		if (dx::d3d12Device)
		{
			log::error("VR doesn't work with the -d3d12 launch option yet. Remove it from your launch options.");
			vr_disabled = true;
			return;
		}

		std::unique_lock runtime_guard(runtime_mutex);
		log_device_once(swap_chain);

		{
			d3d_lock_guard lock(d3d_lock);

			if (restart_requested.exchange(false))
			{
				log::info("Restarting VR");
				runtime.reset();
				next_attempt = {};
			}

			if (!ensure_runtime())
			{
				return;
			}

			if (!runtime->poll_events())
			{
				log::warn("The headset runtime ended the VR session, starting a new one shortly");
				runtime.reset();
				next_attempt = vr_clock::now() + headset_retry_delay;
				return;
			}

			if (recenter_requested.exchange(false))
			{
				runtime->request_recenter();
			}

			if (!runtime->is_session_running())
			{
				return;
			}
		}

		// blocks until the headset wants the next frame; this paces the game to the headset's refresh rate
		if (!runtime->wait_frame())
		{
			return;
		}

		ID3D11Texture2D* back_buffer = nullptr;
		if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
		{
			back_buffer = nullptr;
		}

		{
			d3d_lock_guard lock(d3d_lock);
			runtime->submit_screen_frame(dx::deviceContext, back_buffer, read_screen_params());
		}

		if (back_buffer)
		{
			back_buffer->Release();
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			log::init();
			log::info("IW7-VR {} starting", GIT_DESCRIBE);

			if (utils::flags::has_flag("novr"))
			{
				log::info("Started with -novr, VR is off");
				vr_disabled = true;
			}

			vr_screen_distance = game::Dvar_RegisterFloat("vr_screen_distance", 2.5f, 0.5f, 20.0f, game::DVAR_FLAG_SAVED,
				"VR: distance to the virtual screen, in metres");
			vr_screen_width = game::Dvar_RegisterFloat("vr_screen_width", 3.0f, 0.5f, 30.0f, game::DVAR_FLAG_SAVED,
				"VR: width of the virtual screen, in metres");
			vr_screen_height = game::Dvar_RegisterFloat("vr_screen_height", 0.0f, -5.0f, 5.0f, game::DVAR_FLAG_SAVED,
				"VR: move the virtual screen up (+) or down (-), in metres");
			vr_screen_follow_head = game::Dvar_RegisterBool("vr_screen_follow_head", false, game::DVAR_FLAG_SAVED,
				"VR: lock the virtual screen to your head instead of the room");
			vr_screen_srgb = game::Dvar_RegisterBool("vr_screen_srgb", true, game::DVAR_FLAG_SAVED,
				"VR: treat the game image as sRGB. Turn off if the headset image looks too dark, on if it looks washed out");

			command::add("vr_recenter", []
			{
				recenter_requested = true;
			});

			command::add("vr_restart", []
			{
				log::info("vr_restart requested from the console");
				vr_disabled = false;
				restart_requested = true;
			});

			command::add("vr_status", []
			{
				print_status();
			});
		}

		void pre_destroy() override
		{
			// stop touching the headset while the game shuts down; the runtime cleans up when the process exits
			vr_disabled = true;
		}
	};
}

REGISTER_COMPONENT(vr::component)
