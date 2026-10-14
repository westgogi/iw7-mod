#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "vr.hpp"
#include "vr_log.hpp"
#include "xr_runtime.hpp"
#include "head_tracking.hpp"

#include "component/command.hpp"
#include "component/directx.hpp"
#include "component/console/console.hpp"

#include "game/game.hpp"

#include "version.hpp"

#include <utils/flags.hpp>

// The frame loop. Every frame the image the game is about to present is copied into an OpenXR swapchain:
// - in game, with head tracking on, it is shown as a projection layer from the exact pose it was rendered with
//   (head_tracking.cpp moves the game camera with the headset);
// - in menus, loading screens and videos, it is shown on a big virtual screen floating in front of the player.

namespace vr
{
	namespace
	{
		using vr_clock = std::chrono::steady_clock;

		constexpr auto runtime_retry_delay = 10s;
		constexpr auto headset_retry_delay = 3s;

		// guards `runtime` between the render thread (frame loop) and console commands
		std::timed_mutex runtime_mutex;
		// held briefly by the game thread while it reads the headset pose, and by the render thread whenever it
		// creates or destroys the runtime, so the pose readers never see a half-destroyed session
		std::mutex pose_mutex;
		std::unique_ptr<xr_runtime> runtime;
		vr_clock::time_point next_attempt{};
		bool device_logged = false;

		// ---- freeze diagnostics: the watchdog thread logs if the frame loop is stuck in one place -----------------------

		enum class loop_stage : int
		{
			idle,
			waiting_vr_lock,
			waiting_d3d_lock,
			starting_runtime,
			waiting_frame,
			submitting_frame,
		};

		const char* stage_name(const loop_stage stage)
		{
			switch (stage)
			{
			case loop_stage::waiting_vr_lock: return "waiting for the VR lock";
			case loop_stage::waiting_d3d_lock: return "waiting for the game's D3D lock";
			case loop_stage::starting_runtime: return "starting/polling the OpenXR runtime";
			case loop_stage::waiting_frame: return "xrWaitFrame (waiting for the headset)";
			case loop_stage::submitting_frame: return "submitting the frame to the headset";
			default: return "idle";
			}
		}

		std::atomic<loop_stage> current_stage = loop_stage::idle;
		std::atomic<std::int64_t> stage_since_ms = 0;
		std::atomic<std::int64_t> last_present_ms = 0;
		std::atomic_bool watchdog_stop = false;

		std::int64_t now_ms()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(vr_clock::now().time_since_epoch()).count();
		}

		void set_stage(const loop_stage stage)
		{
			stage_since_ms = now_ms();
			current_stage = stage;
		}

		void watchdog()
		{
			auto reported_stage = false;
			auto reported_present = false;

			while (!watchdog_stop)
			{
				std::this_thread::sleep_for(1s);

				const auto now = now_ms();
				const auto stage = current_stage.load();
				const auto stage_ms = now - stage_since_ms.load();
				if (stage != loop_stage::idle && stage_ms > 3000)
				{
					if (!reported_stage)
					{
						reported_stage = true;
						log::warn("Freeze? The VR frame loop has been stuck {} for {:.1f} s", stage_name(stage), static_cast<double>(stage_ms) / 1000.0);
					}
				}
				else if (reported_stage)
				{
					reported_stage = false;
					log::info("VR frame loop moving again");
				}

				const auto last_present = last_present_ms.load();
				const auto present_gap = now - last_present;
				if (last_present != 0 && present_gap > 5000)
				{
					if (!reported_present)
					{
						reported_present = true;
						log::warn("The game hasn't presented a frame for {:.1f} s (VR loop {})",
							static_cast<double>(present_gap) / 1000.0, stage_name(stage));
					}
				}
				else if (reported_present)
				{
					reported_present = false;
					log::info("The game is presenting frames again");
				}
			}
		}

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
					auto created = std::make_unique<xr_runtime>();
					std::lock_guard _(pose_mutex);
					runtime = std::move(created);
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
				{
					std::lock_guard _(pose_mutex);
					runtime.reset();
				}

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

			console::info("[VR] %s\n", head_tracking::status().data());
		}

		void destroy_runtime()
		{
			std::lock_guard _(pose_mutex);
			runtime.reset();
		}

		void log_threads_once()
		{
			static auto logged = false;
			if (!logged)
			{
				logged = true;
				log::info("Frame loop runs on thread {}", GetCurrentThreadId());
			}
		}

		struct stage_reset
		{
			~stage_reset()
			{
				set_stage(loop_stage::idle);
			}
		};
	}

	bool locate_predicted(const float frames_ahead, view_state& out)
	{
		std::lock_guard _(pose_mutex);
		if (!runtime || vr_disabled)
		{
			return false;
		}

		return runtime->locate(runtime->predicted_time(frames_ahead), out);
	}

	std::uint32_t space_change_count()
	{
		std::lock_guard _(pose_mutex);
		return runtime ? runtime->space_change_count() : 0;
	}

	void on_present(IDXGISwapChain* swap_chain, const HANDLE d3d_lock)
	{
		last_present_ms = now_ms();

		// always take this frame's record, so records stay matched to presents even when VR is idle
		const auto record = head_tracking::pop_frame_record();

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

		const stage_reset reset_stage_on_exit{};
		set_stage(loop_stage::waiting_vr_lock);
		std::unique_lock runtime_guard(runtime_mutex);
		log_device_once(swap_chain);
		log_threads_once();

		{
			set_stage(loop_stage::waiting_d3d_lock);
			d3d_lock_guard lock(d3d_lock);
			set_stage(loop_stage::starting_runtime);

			if (restart_requested.exchange(false))
			{
				log::info("Restarting VR");
				destroy_runtime();
				next_attempt = {};
			}

			if (!ensure_runtime())
			{
				return;
			}

			if (!runtime->poll_events())
			{
				log::warn("The headset runtime ended the VR session, starting a new one shortly");
				destroy_runtime();
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
		set_stage(loop_stage::waiting_frame);
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
			set_stage(loop_stage::waiting_d3d_lock);
			d3d_lock_guard lock(d3d_lock);
			set_stage(loop_stage::submitting_frame);
			runtime->submit_frame(dx::deviceContext, back_buffer, record.head_tracked ? &record.projection : nullptr,
				read_screen_params());
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
				head_tracking::recenter();
			});

			std::thread(watchdog).detach();

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
			watchdog_stop = true;
		}
	};
}

REGISTER_COMPONENT(vr::component)
