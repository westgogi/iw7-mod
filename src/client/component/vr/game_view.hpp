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
