#pragma once

#include "xr_runtime.hpp"

namespace vr::head_tracking
{
	// One entry per finished game frame, in the order the game issued them
	struct frame_record
	{
		bool head_tracked = false;
		projection_info projection{};
		const void* frame_data = nullptr; // the game's frame block this frame was built in (see game_view::render_frame)
		std::uint64_t frame_number = 0;
		bool repeat_previous = false;     // no record for this present: keep showing the last head-tracked pictures
	};

	// Render thread: what the frame being presented (built in `frame_data`, if known) was rendered with
	frame_record pop_frame_record(const void* frame_data);

	// Text for the performance log: how presents were matched to game frames
	std::string pairing_report();

	// Forget the current reference so the next frame recentres on where the player is looking
	void recenter();

	// Text for vr_status
	std::string status();

	// main thread: head-tracked gameplay is on screen, and since when (steady clock ms, 0 = not active)
	bool gameplay_active();
	std::int64_t gameplay_since_ms();
}
