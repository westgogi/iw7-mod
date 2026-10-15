#pragma once

namespace vr
{
	struct view_state;

	// Called on the render thread from the game's present hook (component/gui/gui.cpp), right before the
	// game's own Present. `d3d_lock` is the game's mutex for the D3D11 immediate context.
	void on_present(IDXGISwapChain* swap_chain, HANDLE d3d_lock);

	// Any thread: headset pose predicted `frames_ahead` frames after the frame the runtime last asked for.
	// Returns false while VR isn't running.
	bool locate_predicted(float frames_ahead, view_state& out);

	// Any thread: changes whenever the runtime recentres (e.g. holding the Meta button)
	std::uint32_t space_change_count();

	// Any thread: a VR session has run at some point since the game started (and VR hasn't been switched off since)
	bool has_run();
}
