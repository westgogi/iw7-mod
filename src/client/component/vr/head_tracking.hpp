#pragma once

#include "xr_runtime.hpp"

namespace vr::head_tracking
{
	// One entry per finished game frame, in the order the game issued them
	struct frame_record
	{
		bool head_tracked = false;
		projection_info projection{};
	};

	// Render thread: what the frame being presented was rendered with
	frame_record pop_frame_record();

	// Forget the current reference so the next frame recentres on where the player is looking
	void recenter();

	// Text for vr_status
	std::string status();

	// main thread: head-tracked gameplay is on screen, and since when (steady clock ms, 0 = not active)
	bool gameplay_active();
	std::int64_t gameplay_since_ms();
}
