#pragma once

// Game-side camera data for the Steam build of iw7_ship.exe, found by analysing a memory dump (vr_dump_game).
// See the "Reverse-engineering notes" in the project plan for how each address was identified.
namespace vr::game_view
{
	// Hands the finished camera to the renderer. Both frame functions call it once per frame after the view (and the
	// viewmodel) is computed and before the sound listener, renderer and effects read it:
	//   0x14026CB50 (multiplayer CG_DrawActiveFrame) and 0x1402F8150 (single-player CG_DrawActiveFrame).
	// void (int localClientNum, refdef_t* refdef, RefdefView* view, float zNear, int, int, bool skip)
	constexpr std::uintptr_t submit_view = 0x140AC28E0;

	// Called by the input code once per user command with the view angles about to be stored in the client state:
	// (const aim_input* in, aim_output* out); out holds {float pitch, float yaw, ...}
	constexpr std::uintptr_t aim_adjust = 0x1405BC540;
	constexpr std::size_t aim_input_local_client = 0x30;

	// Builds a reversed-Z infinite projection matrix: void (float tanHalfFovX, float tanHalfFovY, float zNear, float* out)
	// out = {1/tanX, 1/tanY, m23 = 1, m32 = zNear}. Every perspective view the renderer draws goes through it.
	constexpr std::uintptr_t projection_matrix = 0x140CE0E90;

	// bool (int controllerIndex): the profile's "gamepad enabled" option (profile byte 0xC0, "gpadEnabled").
	// The game ignores the gamepad while it returns false.
	constexpr std::uintptr_t gamepad_enabled = 0x140337800;

	// Frame data handed from the game thread to the render thread. The game thread builds a frame in the block
	// *frontend_frame_data points to (set by R_ToggleSmpFrame, cleared once the frame is handed over); the render thread
	// runs render_frame(block) for it, which ends with the present. Matching the block tells us which game frame is
	// being presented, whatever the queueing in between.
	constexpr std::uintptr_t frontend_frame_data = 0x148F73088;
	constexpr std::uintptr_t render_frame = 0x140E57390; // void (FrameData*), render thread

	// Player profiles (one per controller): gamepad options. With gpadEnabled off the game runs "exec buttons_unbind"
	// for the gamepad, so a pad does nothing even though it is read.
	constexpr std::uintptr_t profiles = 0x1452456D0;
	constexpr std::size_t profile_size = 0x20E8;
	constexpr std::size_t profile_gamepad_enabled = 0xC0; // bool "gpadEnabled"

	// void (int controllerIndex): runs "exec <gpadButtonsConfig>" and "exec <gpadSticksConfig>" for the profile when its
	// gamepad is enabled, otherwise "exec buttons_unbind" (what the options menu calls after toggling the gamepad)
	constexpr std::uintptr_t apply_gamepad_bindings = 0x1409C1440;

	// Renderer dvars (pointers to the dvar_t*), for the effects that don't work in VR
	constexpr std::uintptr_t dvar_r_post_aa = 0x1483E8B58;      // enum None, FXAA, SMAA 1x, Filmic SMAA 1x, SMAA T2x, Filmic SMAA T2x
	constexpr std::uintptr_t dvar_r_mb_enable = 0x1483E9320;    // enum Off, On (all ents), ... (cheat-protected)
	constexpr std::uintptr_t dvar_r_mb_enable_a = 0x1483E9328;  // bool, the saved motion blur setting
	constexpr std::uintptr_t dvar_r_dof_enable = 0x1483E9148;   // bool

	// ---- window and back buffer size (main thread) ---------------------------------------------------------------------
	// R_SetWndParms fills a GfxWindowParms (window_parms below) at startup (0x140E05460) and whenever the window is
	// resized or the video settings change (0x140E063E0): window_size, then the position, then window_aspect; the scene
	// size is display size x the "render resolution" percentage; then the window is created/placed, the swap chain is
	// created (or ResizeBuffers'd) at the display size and vidConfig is filled from the parms.

	// void (window_parms*): display size = vid_width/vid_height (windowed; the client area once the window exists),
	// the monitor (borderless) or the display mode (fullscreen); clamped to 320x180..16384
	constexpr std::uintptr_t window_size = 0x140E07E40;
	// void (window_parms*): aspect ratio from r_aspectRatio; "auto" = display width / height
	constexpr std::uintptr_t window_aspect = 0x140E07A30;
	// bool (window_parms*): startup: window style, then SetWindowPlacement to the display size (+ frame)
	constexpr std::uintptr_t create_window = 0x140E03F30;
	// void (window_parms*): the same after a video settings change
	constexpr std::uintptr_t place_window = 0x140E06830;
	// void (POINT*): GetCursorPos + ScreenToClient; the menus read the mouse through it and compare it with the display size
	constexpr std::uintptr_t cursor_position = 0x140D31360;
	// int: set to 1 to run the window-resize path on the next frame (what WM_SIZE does); the main loop clears it
	constexpr std::uintptr_t video_resize_requested = 0x1477A0318;
	constexpr std::uintptr_t game_window = 0x1477A02D0; // HWND

	struct window_parms
	{
		HWND hwnd;
		int x;
		int y;
		int unknown_10;
		int mode; // 0 = fullscreen, 1 = fullscreen window (borderless), 2 = windowed
		int aspect_index;
		float aspect; // width / height the UI is laid out for
		std::uint16_t shadow_tile_sizes[2];
		int unknown_24;
		int unknown_28;
		int scene_width; // the 3D scene, display size x render resolution %
		int scene_height;
		int display_width; // swap chain and UI
		int display_height;
	};

	static_assert(offsetof(window_parms, mode) == 0x14);
	static_assert(offsetof(window_parms, aspect) == 0x1C);
	static_assert(offsetof(window_parms, scene_width) == 0x2C);
	static_assert(offsetof(window_parms, display_width) == 0x34);

	// vidConfig_t, filled from window_parms after every change
	struct vid_config
	{
		int scene_width;
		int scene_height;
		int display_width;
		int display_height;
		std::uint16_t unknown_10[2];
		int mode;
		int aspect_index;
		float aspect;               // width / height the UI is laid out for
		float scene_aspect;         // scene width / height in pixels
		float scene_pixel_aspect;   // aspect * scene height / scene width (1 = square pixels)
		float display_pixel_aspect; // aspect * display height / display width
	};

	static_assert(offsetof(vid_config, display_width) == 0x08);
	static_assert(offsetof(vid_config, aspect) == 0x1C);

	inline const vid_config& current_vid_config()
	{
		return *reinterpret_cast<const vid_config*>(0x148B9D7A8);
	}

	// refdef_t lives at cg_s + 0x4B38; RefdefView at refdef + 0x10
	struct refdef_view
	{
		float tan_half_fov_x;
		float tan_half_fov_y;
		float origin[3];
		float axis[3][3]; // forward, left, up
		float z_near;     // 0 = use the renderer's default (r_zPlanes)
		// Second FOV pair. The frame's view setup copies it into the renderer's view info (+0x160) and the scene passes
		// build their projection from it, so this is what the world is actually drawn with. The game sets it from
		// the "dual FOV" (viewmodel) FOV while aiming down sights, otherwise it copies tan_half_fov_x/y into it before
		// submit_view runs (cgame 0x140888AB0 / 0x1408B4550).
		float draw_tan_half_fov[2];
	};

	static_assert(offsetof(refdef_view, origin) == 0x08);
	static_assert(offsetof(refdef_view, axis) == 0x14);
	static_assert(offsetof(refdef_view, z_near) == 0x38);
	static_assert(offsetof(refdef_view, draw_tan_half_fov) == 0x3C);

	inline refdef_view* view(std::uint8_t* refdef)
	{
		return reinterpret_cast<refdef_view*>(refdef + 0x10);
	}

	// copies CG_CalcViewValues makes for the effects camera (cg + 0x49188 origin, cg + 0x49194 angles)
	inline float* effects_origin(std::uint8_t* refdef)
	{
		return reinterpret_cast<float*>(refdef + (0x49188 - 0x4B38));
	}

	inline float* effects_angles(std::uint8_t* refdef)
	{
		return reinterpret_cast<float*>(refdef + (0x49194 - 0x4B38));
	}
}
