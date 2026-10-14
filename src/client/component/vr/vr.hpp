#pragma once

namespace vr
{
	// Called on the render thread from the game's present hook (component/gui/gui.cpp), right before the
	// game's own Present. `d3d_lock` is the game's mutex for the D3D11 immediate context.
	void on_present(IDXGISwapChain* swap_chain, HANDLE d3d_lock);
}
