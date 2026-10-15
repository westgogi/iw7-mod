#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "render_size.hpp"
#include "game_view.hpp"
#include "vr_log.hpp"

#include "game/game.hpp"

#include <utils/flags.hpp>
#include <utils/hook.hpp>

// See render_size.hpp. How the game sizes its picture (all on the main thread):
//  - window_size works out the display size (vid_width/vid_height, the window's client area, or the monitor). Our
//    hook replaces it with the headset size, remembering the window's own size.
//  - window_aspect, create_window and place_window shape and place the window from the display size; while they run
//    they're shown the window's own size, so the window stays as it was and the menus keep its shape.
//  - The swap chain, render targets and vidConfig then follow the display size. The window shows the bigger picture
//    scaled down (the game's swap chain stretches to the window).
//  - The menus compare the mouse (in window pixels) with the display size, so the cursor is scaled to display pixels.

namespace vr::render_size
{
	namespace
	{
		game::dvar_t* vr_resolution = nullptr;
		game::dvar_t* vr_render_width = nullptr;
		game::dvar_t* vr_render_height = nullptr;

		utils::hook::detour window_size_hook;
		utils::hook::detour window_aspect_hook;
		utils::hook::detour create_window_hook;
		utils::hook::detour place_window_hook;
		utils::hook::detour cursor_position_hook;

		bool vr_off = false; // started with -novr

		// main thread
		int applied_width = 0; // the display size we last gave the game (0 = it used its own)
		int applied_height = 0;
		int window_width = 0; // the size the game would have used: the window's own
		int window_height = 0;
		bool fullscreen_reported = false;
		std::pair<int, int> requested{-1, -1};
		game_view::vid_config logged_config{};

		// the display size the game should use: the saved headset size while VR is wanted, else {0, 0} (its own)
		std::pair<int, int> wanted_size()
		{
			if (vr_off || !vr_resolution || !vr_render_width || !vr_render_height || vr_resolution->current.value <= 0.0f)
			{
				return {0, 0};
			}

			const auto width = vr_render_width->current.integer;
			const auto height = vr_render_height->current.integer;
			if (width < 320 || height < 180)
			{
				return {0, 0};
			}

			return {width, height};
		}

		void window_size_stub(game_view::window_parms* parms)
		{
			window_size_hook.invoke<void>(parms);

			const auto [width, height] = wanted_size();
			if (width <= 0 || parms->mode == 0)
			{
				if (width > 0 && !fullscreen_reported)
				{
					fullscreen_reported = true;
					log::warn("The game is in exclusive fullscreen, where it can only render at the monitor's resolution. "
						"For a sharper picture in the headset, set Display Mode to windowed or borderless (fullscreen window).");
				}

				if (applied_width != 0)
				{
					log::info("The game renders at its window's size again ({}x{})", parms->display_width, parms->display_height);
				}

				applied_width = 0;
				applied_height = 0;
				return;
			}

			window_width = parms->display_width;
			window_height = parms->display_height;
			parms->display_width = width;
			parms->display_height = height;

			if (width != applied_width || height != applied_height)
			{
				log::info("The game renders at {}x{} for the headset; the window stays {}x{}", width, height, window_width, window_height);
			}

			applied_width = width;
			applied_height = height;
		}

		// While the game shapes or places its window, it sees the window's own size instead of the headset size
		class window_size_scope
		{
		public:
			explicit window_size_scope(game_view::window_parms* parms)
				: parms_(parms)
			{
				if (parms && applied_width > 0 && window_width > 0
					&& parms->display_width == applied_width && parms->display_height == applied_height)
				{
					this->width_ = parms->display_width;
					this->height_ = parms->display_height;
					parms->display_width = window_width;
					parms->display_height = window_height;
					this->active_ = true;
				}
			}

			~window_size_scope()
			{
				if (this->active_)
				{
					this->parms_->display_width = this->width_;
					this->parms_->display_height = this->height_;
				}
			}

			window_size_scope(const window_size_scope&) = delete;
			window_size_scope& operator=(const window_size_scope&) = delete;

		private:
			game_view::window_parms* parms_;
			int width_ = 0;
			int height_ = 0;
			bool active_ = false;
		};

		void window_aspect_stub(game_view::window_parms* parms)
		{
			const window_size_scope scope(parms);
			window_aspect_hook.invoke<void>(parms);
		}

		bool create_window_stub(game_view::window_parms* parms)
		{
			const window_size_scope scope(parms);
			return create_window_hook.invoke<bool>(parms);
		}

		void place_window_stub(game_view::window_parms* parms)
		{
			const window_size_scope scope(parms);
			place_window_hook.invoke<void>(parms);
		}

		void cursor_position_stub(POINT* point)
		{
			cursor_position_hook.invoke<void>(point);

			if (!point || applied_width <= 0)
			{
				return;
			}

			const auto window = *reinterpret_cast<HWND*>(game_view::game_window);
			RECT client{};
			if (!window || !GetClientRect(window, &client))
			{
				return;
			}

			const auto client_width = client.right - client.left;
			const auto client_height = client.bottom - client.top;
			const auto& config = game_view::current_vid_config();
			if (client_width <= 0 || client_height <= 0 || config.display_width <= 0 || config.display_height <= 0)
			{
				return;
			}

			point->x = MulDiv(point->x, config.display_width, client_width);
			point->y = MulDiv(point->y, config.display_height, client_height);
		}

		int round_up(const float value, const int multiple)
		{
			return static_cast<int>(std::ceil(value / static_cast<float>(multiple))) * multiple;
		}

		void log_config_change()
		{
			const auto& config = game_view::current_vid_config();
			if (config.display_width == logged_config.display_width && config.display_height == logged_config.display_height
				&& config.scene_width == logged_config.scene_width && config.scene_height == logged_config.scene_height
				&& config.aspect == logged_config.aspect)
			{
				return;
			}

			logged_config = config;
			log::info("Game picture: {}x{} (3D scene {}x{}), laid out for a {:.3f} aspect ratio", config.display_width,
				config.display_height, config.scene_width, config.scene_height, config.aspect);

			if (applied_width > 0 && (config.scene_width < config.display_width || config.scene_height < config.display_height))
			{
				log::warn("The game's Render Resolution is below 100%, so the 3D scene is drawn smaller and stretched up. "
					"Set it to 100% (or use vr_resolution) for a sharper picture.");
			}
		}
	}

	void update(const headset_view& view)
	{
		if (vr_off || !vr_resolution || !vr_render_width || !vr_render_height)
		{
			return;
		}

		log_config_change();

		const auto scale = vr_resolution->current.value;
		if (scale > 0.0f && view.eye_width > 0 && view.eye_height > 0)
		{
			// the picture: one symmetric frustum covering both eyes, exactly as head_tracking draws it
			auto tan_x = 0.0f;
			auto tan_y = 0.0f;
			for (const auto& fov : view.eye_fov)
			{
				tan_x = std::max({tan_x, std::tan(-fov.angleLeft), std::tan(fov.angleRight)});
				tan_y = std::max({tan_y, std::tan(fov.angleUp), std::tan(-fov.angleDown)});
			}

			// the headset's pixel density: its recommended resolution over one eye's field of view (in tangent units, as
			// a flat picture is), so each eye's part of our picture gets about as many pixels as the headset asks for
			const auto& eye = view.eye_fov[0];
			const auto span_x = std::tan(eye.angleRight) - std::tan(eye.angleLeft);
			const auto span_y = std::tan(eye.angleUp) - std::tan(eye.angleDown);
			if (tan_x > 0.1f && tan_y > 0.1f && span_x > 0.1f && span_y > 0.1f)
			{
				const auto density = std::max(static_cast<float>(view.eye_width) / span_x, static_cast<float>(view.eye_height) / span_y);
				const auto limit = static_cast<int>(view.max_size > 0 ? std::min<std::uint32_t>(view.max_size, 16384) : 4096);
				const auto width = std::clamp(round_up(2.0f * tan_x * density * scale, 8), 320, limit);
				const auto height = std::clamp(round_up(2.0f * tan_y * density * scale, 8), 180, limit);

				if (width != vr_render_width->current.integer || height != vr_render_height->current.integer)
				{
					log::info("Headset render size: {}x{} (headset asks for {}x{} per eye = {:.0f} pixels per unit; picture covers "
						"tan {:.3f} x {:.3f}; vr_resolution {:.2f})", width, height, view.eye_width, view.eye_height, density,
						tan_x, tan_y, scale);
					game::Dvar_SetInt(vr_render_width, width);
					game::Dvar_SetInt(vr_render_height, height);
				}
			}
		}

		// if the game isn't rendering at the size we want, ask it to set its window up again (once per size)
		const auto wanted = wanted_size();
		const auto current = std::pair{applied_width, applied_height};
		if (wanted != current && wanted != requested)
		{
			requested = wanted;
			log::info("Asking the game to switch to {}", wanted.first > 0
				? std::format("{}x{}", wanted.first, wanted.second) : std::string("its window's size"));
			*reinterpret_cast<volatile int*>(game_view::video_resize_requested) = 1;
		}
	}

	float ui_aspect()
	{
		const auto aspect = game_view::current_vid_config().aspect;
		return aspect > 0.1f && aspect < 10.0f ? aspect : 0.0f;
	}

	std::string status()
	{
		const auto& config = game_view::current_vid_config();
		return std::format("picture {}x{} (scene {}x{}, window {}x{}, vr_resolution {:.2f})", config.display_width,
			config.display_height, config.scene_width, config.scene_height, applied_width > 0 ? window_width : config.display_width,
			applied_width > 0 ? window_height : config.display_height, vr_resolution ? vr_resolution->current.value : 0.0f);
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

			vr_off = utils::flags::has_flag("novr");

			vr_resolution = game::Dvar_RegisterFloat("vr_resolution", 1.0f, 0.0f, 2.0f, game::DVAR_FLAG_SAVED,
				"VR: render resolution as a multiple of what the headset asks for (1 = as many pixels as the headset shows in the "
				"middle of the view). Lower it if the frame rate drops. 0 = render at the window's size");
			vr_render_width = game::Dvar_RegisterInt("vr_render_width", 0, 0, 16384, game::DVAR_FLAG_SAVED,
				"VR: the width the game renders at in VR, worked out from the headset (set automatically)");
			vr_render_height = game::Dvar_RegisterInt("vr_render_height", 0, 0, 16384, game::DVAR_FLAG_SAVED,
				"VR: the height the game renders at in VR, worked out from the headset (set automatically)");

			window_size_hook.create(game_view::window_size, reinterpret_cast<void*>(window_size_stub));
			window_aspect_hook.create(game_view::window_aspect, reinterpret_cast<void*>(window_aspect_stub));
			create_window_hook.create(game_view::create_window, reinterpret_cast<void*>(create_window_stub));
			place_window_hook.create(game_view::place_window, reinterpret_cast<void*>(place_window_stub));
			cursor_position_hook.create(game_view::cursor_position, reinterpret_cast<void*>(cursor_position_stub));
		}
	};
}

REGISTER_COMPONENT(vr::render_size::component)
