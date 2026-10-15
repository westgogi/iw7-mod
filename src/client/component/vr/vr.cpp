#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "vr.hpp"
#include "vr_log.hpp"
#include "xr_runtime.hpp"
#include "head_tracking.hpp"
#include "input.hpp"
#include "game_view.hpp"
#include "render_size.hpp"

#include "component/command.hpp"
#include "component/directx.hpp"
#include "component/scheduler.hpp"
#include "component/console/console.hpp"

#include "game/game.hpp"

#include "version.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>

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

		// ---- GPU time per game frame --------------------------------------------------------------------------------
		// Timestamps when the render thread starts drawing a frame and right before it presents it, read back a few
		// frames later without waiting. Render thread only, with the game's D3D lock held.

		class gpu_timer
		{
		public:
			~gpu_timer()
			{
				this->release();
			}

			void begin(ID3D11Device* device, ID3D11DeviceContext* context)
			{
				if (device != this->device_)
				{
					this->release();
					this->device_ = device;
					this->ready_ = this->create();
				}

				if (!this->ready_)
				{
					return;
				}

				if (this->open_ >= 0)
				{
					// the previous frame was never presented; close its measurement and drop it
					context->End(this->slots_[this->open_].disjoint);
					this->open_ = -1;
				}

				this->next_ = (this->next_ + 1) % slot_count;
				auto& slot = this->slots_[this->next_];
				slot.pending = false;
				context->Begin(slot.disjoint);
				context->End(slot.start);
				this->open_ = this->next_;
			}

			void end(ID3D11DeviceContext* context)
			{
				if (!this->ready_ || this->open_ < 0)
				{
					return;
				}

				auto& slot = this->slots_[this->open_];
				context->End(slot.finish);
				context->End(slot.disjoint);
				slot.pending = true;
				this->open_ = -1;

				this->collect(context);
			}

			// average and worst GPU milliseconds of the frames measured since the last call
			bool take(double& average, double& worst)
			{
				if (this->frames_ == 0)
				{
					return false;
				}

				average = this->total_ms_ / static_cast<double>(this->frames_);
				worst = this->worst_ms_;
				this->total_ms_ = 0.0;
				this->worst_ms_ = 0.0;
				this->frames_ = 0;
				return true;
			}

		private:
			static constexpr int slot_count = 6;

			struct slot
			{
				ID3D11Query* disjoint = nullptr;
				ID3D11Query* start = nullptr;
				ID3D11Query* finish = nullptr;
				bool pending = false;
			};

			ID3D11Device* device_ = nullptr;
			bool ready_ = false;
			std::array<slot, slot_count> slots_{};
			int next_ = 0;
			int open_ = -1;
			double total_ms_ = 0.0;
			double worst_ms_ = 0.0;
			std::uint64_t frames_ = 0;

			bool create()
			{
				if (!this->device_)
				{
					return false;
				}

				D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
				D3D11_QUERY_DESC timestamp_desc{D3D11_QUERY_TIMESTAMP, 0};
				for (auto& slot : this->slots_)
				{
					if (FAILED(this->device_->CreateQuery(&disjoint_desc, &slot.disjoint))
						|| FAILED(this->device_->CreateQuery(&timestamp_desc, &slot.start))
						|| FAILED(this->device_->CreateQuery(&timestamp_desc, &slot.finish)))
					{
						log::warn("Couldn't create GPU timing queries; the log won't show GPU frame times");
						this->release();
						return false;
					}
				}

				return true;
			}

			void release()
			{
				for (auto& slot : this->slots_)
				{
					for (auto** query : {&slot.disjoint, &slot.start, &slot.finish})
					{
						if (*query)
						{
							(*query)->Release();
							*query = nullptr;
						}
					}

					slot.pending = false;
				}

				this->ready_ = false;
				this->open_ = -1;
			}

			void collect(ID3D11DeviceContext* context)
			{
				for (auto& slot : this->slots_)
				{
					if (!slot.pending)
					{
						continue;
					}

					D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
					UINT64 start = 0;
					UINT64 finish = 0;
					if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
						|| context->GetData(slot.start, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
						|| context->GetData(slot.finish, &finish, sizeof(finish), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
					{
						continue; // not finished on the GPU yet
					}

					slot.pending = false;
					if (disjoint.Disjoint || disjoint.Frequency == 0 || finish < start)
					{
						continue;
					}

					const auto ms = static_cast<double>(finish - start) * 1000.0 / static_cast<double>(disjoint.Frequency);
					this->total_ms_ += ms;
					this->worst_ms_ = std::max(this->worst_ms_, ms);
					++this->frames_;
				}
			}
		};

		gpu_timer frame_timer;

		// the game frame block the render thread is drawing and presenting right now (render thread only)
		thread_local const void* presenting_frame_data = nullptr;
		utils::hook::detour render_frame_hook;

		void render_frame_stub(void* frame_data)
		{
			presenting_frame_data = frame_data;

			// start this frame's GPU timing; never wait for the D3D lock here (whoever holds it may be waiting for us)
			if (!vr_disabled && dx::device && dx::deviceContext && !dx::d3d12Device)
			{
				const auto lock = *reinterpret_cast<HANDLE*>(0x148B1BC98);
				const auto taken = lock ? WaitForSingleObject(lock, 0) : WAIT_TIMEOUT;
				if (taken == WAIT_OBJECT_0 || taken == WAIT_ABANDONED)
				{
					frame_timer.begin(dx::device, dx::deviceContext);
					ReleaseMutex(lock);
				}
			}

			render_frame_hook.invoke<void>(frame_data);
			presenting_frame_data = nullptr;
		}
		std::atomic_bool session_has_run = false;
		std::atomic_bool restart_requested = false;
		std::atomic_bool recenter_requested = false;

		game::dvar_t* vr_screen_distance = nullptr;
		game::dvar_t* vr_screen_width = nullptr;
		game::dvar_t* vr_screen_height = nullptr;
		game::dvar_t* vr_screen_follow_head = nullptr;
		game::dvar_t* vr_screen_srgb = nullptr;
		game::dvar_t* vr_refresh_rate = nullptr;

		// performance log, every 10 s while the headset shows frames
		constexpr auto performance_log_interval = 10s;
		vr_clock::time_point last_performance_log{};
		std::uint64_t presents_since_log = 0;
		std::uint64_t submits_since_log = 0;
		std::uint64_t tracked_since_log = 0;

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

			params.aspect = render_size::ui_aspect();
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

			console::info("[VR] IW7-VR %s\n", GIT_DESCRIBE);
			if (runtime)
			{
				console::info("[VR] %s\n", runtime->status().data());
			}
			else
			{
				console::info("[VR] %s\n", vr_disabled ? "off (see iw7-vr.log in the game folder)" : "waiting for the headset runtime");
			}

			console::info("[VR] %s\n", head_tracking::status().data());
			console::info("[VR] %s\n", input::status().data());
			console::info("[VR] %s\n", render_size::status().data());
		}

		// main thread, once a second: tell render_size what the headset wants
		void update_render_size()
		{
			render_size::headset_view view{};
			{
				std::lock_guard _(pose_mutex);
				view_state state{};
				if (runtime && !vr_disabled && runtime->is_session_running()
					&& runtime->locate(runtime->predicted_time(1.0f), state))
				{
					view.eye_fov[0] = state.eye_fov[0];
					view.eye_fov[1] = state.eye_fov[1];
					view.eye_width = runtime->recommended_eye_width();
					view.eye_height = runtime->recommended_eye_height();
					view.max_size = runtime->max_image_size();
				}
			}

			render_size::update(view);
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

	namespace
	{
		// Whatever path on_present takes, the controllers end up published once per present: inactive unless the
		// frame loop managed to read them this frame
		struct controller_publisher
		{
			input::controller_state state{};

			~controller_publisher()
			{
				input::publish(this->state);
			}
		};

		void log_performance(xr_runtime& xr)
		{
			const auto now = vr_clock::now();
			if (last_performance_log == vr_clock::time_point{})
			{
				last_performance_log = now;
				return;
			}

			const auto elapsed = std::chrono::duration<double>(now - last_performance_log).count();
			if (elapsed < std::chrono::duration<double>(performance_log_interval).count())
			{
				return;
			}

			if (submits_since_log > 0)
			{
				const auto counters = xr.performance_report();
				auto gpu_average = 0.0;
				auto gpu_worst = 0.0;
				const auto gpu = frame_timer.take(gpu_average, gpu_worst)
					? std::format("GPU {:.2f} ms per frame (worst {:.2f}) | ", gpu_average, gpu_worst) : std::string();
				log::info("Perf: game {:.1f} fps, headset frames {:.1f}/s ({:.0f}% head-tracked) | {}{}{}{}",
					static_cast<double>(presents_since_log) / elapsed, static_cast<double>(submits_since_log) / elapsed,
					100.0 * static_cast<double>(tracked_since_log) / static_cast<double>(submits_since_log),
					gpu, head_tracking::pairing_report(), counters.empty() ? "" : " | runtime: ", counters);
			}

			last_performance_log = now;
			presents_since_log = 0;
			submits_since_log = 0;
			tracked_since_log = 0;
		}
	}

	namespace
	{
		// "IW7-VR <build>" on screen, so it's easy to check which build is running: top right on the virtual screen, and
		// for the first 10 s of head-tracked play up in the middle of the view (the corners are out of sight there).
		// Main thread (renderer pipeline, before R_EndFrame).
		void draw_version()
		{
			const auto* placement = game::ScrPlace_GetViewPlacement();
			if (!placement)
			{
				return;
			}

			const auto width = placement->realViewportSize[0];
			const auto height = placement->realViewportSize[1];
			if (width <= 0.0f || height <= 0.0f)
			{
				return;
			}

			auto* font = game::R_RegisterFont("fonts/fira_mono_regular.ttf", 24);
			if (!font)
			{
				return;
			}

			static constexpr auto text = "IW7-VR " GIT_DESCRIBE;
			const auto gameplay = head_tracking::gameplay_active();

			// The same size relative to the picture whatever its resolution. On the virtual screen the picture is shown
			// at the shape the game laid it out for, which makes its pixels wider than tall when rendering at the
			// headset's resolution; in the headset view they're square.
			const auto pixel_aspect = game_view::current_vid_config().display_pixel_aspect;
			const auto y_scale = std::max(1.0f, height / 1200.0f);
			const auto x_scale = y_scale * (!gameplay && pixel_aspect > 0.2f && pixel_aspect < 5.0f ? 1.0f / pixel_aspect : 1.0f);
			const auto text_width = static_cast<float>(game::R_TextWidth(text, std::numeric_limits<int>::max(), font)) * x_scale;

			float x;
			float y;
			if (gameplay)
			{
				const auto since = head_tracking::gameplay_since_ms();
				if (since == 0 || now_ms() - since > 10'000)
				{
					return;
				}

				x = (width - text_width) * 0.5f;
				y = height * 0.32f;
			}
			else
			{
				x = width - text_width - 24.0f * x_scale;
				y = (24.0f + static_cast<float>(font->pixelHeight)) * y_scale;
			}

			float color[4]{1.0f, 1.0f, 1.0f, 0.6f};
			game::R_AddCmdDrawText(text, std::numeric_limits<int>::max(), font, x, y, x_scale, y_scale, 0.0f, color, 0);
		}
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

	bool has_run()
	{
		return session_has_run && !vr_disabled;
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
		const auto record = head_tracking::pop_frame_record(presenting_frame_data);
		controller_publisher controllers{};
		++presents_since_log;

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

			// the game has issued all of this frame's drawing
			frame_timer.end(dx::deviceContext);

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

		session_has_run = true;

		runtime->sync_input(controllers.state);
		{
			auto left = 0.0f;
			auto right = 0.0f;
			input::rumble(left, right);
			runtime->apply_haptics(left, right);
		}

		if (vr_refresh_rate)
		{
			runtime->request_refresh_rate(static_cast<float>(vr_refresh_rate->current.integer));
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
				read_screen_params(), record.repeat_previous);
		}

		if (back_buffer)
		{
			back_buffer->Release();
		}

		++submits_since_log;
		if (record.head_tracked)
		{
			++tracked_since_log;
		}

		log_performance(*runtime);
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
			vr_refresh_rate = game::Dvar_RegisterInt("vr_refresh_rate", 0, 0, 144, game::DVAR_FLAG_SAVED,
				"VR: ask the headset for this refresh rate in Hz (e.g. 72, 80, 90, 120); 0 = leave it as set in the headset software");

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

			scheduler::loop(draw_version, scheduler::renderer);
			scheduler::loop(update_render_size, scheduler::main, 1s);
			render_frame_hook.create(game_view::render_frame, reinterpret_cast<void*>(render_frame_stub));
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
