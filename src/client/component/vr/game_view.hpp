#pragma once

// Game-side camera data for the Steam build of iw7_ship.exe, found by analysing a memory dump (vr_dump_game).
// See the "Reverse-engineering notes" in the project plan for how each address was identified.
namespace vr::game_view
{
	// CG_DrawActiveFrame -> CG_CalcViewValues(int localClientNum) builds the camera (cg_s::refdef) every frame
	constexpr std::uintptr_t cg_calc_view_values = 0x14026B7E0;

	// Called by the input code once per user command with the view angles about to be stored in the client state:
	// (const aim_input* in, aim_output* out); out holds {float pitch, float yaw, ...}
	constexpr std::uintptr_t aim_adjust = 0x1405BC540;
	constexpr std::size_t aim_input_local_client = 0x30;

	// CG_GetLocalClientGlobals (0x140242BF0): cg = *(cg_s**)0x141FDBEF0 + localClientNum * 0xFDEA8.
	// Unlike iw7-mod's game::cg (an encrypted pointer array), this is a plain pointer; it's null outside of a map.
	inline std::uint8_t* cg(const int local_client)
	{
		auto* base = *reinterpret_cast<std::uint8_t**>(0x141FDBEF0);
		return base ? base + static_cast<std::size_t>(local_client) * 0xFDEA8 : nullptr;
	}

	struct refdef_view
	{
		float tan_half_fov_x;
		float tan_half_fov_y;
		float origin[3];
		float axis[3][3]; // forward, left, up
	};

	inline refdef_view* view(std::uint8_t* cg)
	{
		return reinterpret_cast<refdef_view*>(cg + 0x4B48); // cg_s::refdef (0x4B38) + 0x10
	}

	// cg_s::refdefViewAngles: pitch, yaw, roll the game computed the camera from
	inline float* view_angles(std::uint8_t* cg)
	{
		return reinterpret_cast<float*>(cg + 0x49158);
	}

	// predictedPlayerState (cg + 0x8): delta_angles at +0x80, viewangles at +0x1B8
	inline float* player_delta_angles(std::uint8_t* cg)
	{
		return reinterpret_cast<float*>(cg + 0x8 + 0x80);
	}

	// copies CG_CalcViewValues makes for the effects camera: final origin, final angles
	inline float* effects_origin(std::uint8_t* cg)
	{
		return reinterpret_cast<float*>(cg + 0x49188);
	}

	inline float* effects_angles(std::uint8_t* cg)
	{
		return reinterpret_cast<float*>(cg + 0x49194);
	}
}
