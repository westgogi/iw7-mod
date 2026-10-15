#pragma once

#include "xr_runtime.hpp"

// The game renders at its "display size", which is normally its window's size. In the headset each eye sees roughly
// 90 x 90 degrees, so a 1920x1200 window leaves each eye far fewer pixels than the headset can show, especially
// vertically (the picture has to be almost square to cover both eyes). While VR is on, the game renders at a size
// worked out from the headset's own recommended resolution instead (vr_resolution scales it). The window on the
// monitor keeps its size and shows the picture scaled to fit, and the menus keep the window's shape.
//
// The size is saved (vr_render_width/height) so the game starts at it next time; the first time, or when it changes,
// the game is asked to set its window up again (the same thing it does when the window is resized).
namespace vr::render_size
{
	struct headset_view
	{
		XrFovf eye_fov[2]{};
		std::uint32_t eye_width = 0;  // the runtime's recommended resolution per eye
		std::uint32_t eye_height = 0;
		std::uint32_t max_size = 0;   // largest image the runtime accepts (0 = unknown)
	};

	// main thread, about once a second while the headset is running
	void update(const headset_view& view);

	// any thread: width / height the game laid its picture out for (its menus and HUD); 0 = unknown
	float ui_aspect();

	// text for vr_status
	std::string status();
}
