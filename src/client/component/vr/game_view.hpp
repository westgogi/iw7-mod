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

	// refdef_t lives at cg_s + 0x4B38; RefdefView at refdef + 0x10
	struct refdef_view
	{
		float tan_half_fov_x;
		float tan_half_fov_y;
		float origin[3];
		float axis[3][3]; // forward, left, up
	};

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
