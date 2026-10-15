#pragma once

// VR controllers. The frame loop reads the Touch controllers through OpenXR every headset frame and publishes them
// here; input.cpp then shows them to the game as an Xbox controller (it replaces the game's XInput imports), so
// movement, buttons, menus and rumble use the game's own gamepad support. Turning is done by head_tracking.cpp
// (smooth or snap turn) instead of the game's stick look, which has acceleration and aim slow-down.
namespace vr::input
{
	struct controller_state
	{
		bool connected = false; // show the game a gamepad (the controllers have been used during this VR session)
		bool active = false;    // inputs are live: controllers on and the VR session has input focus
		float left_stick[2]{};  // x: right, y: forward/up; -1..1
		float right_stick[2]{};
		float left_trigger = 0.0f;
		float right_trigger = 0.0f;
		float left_grip = 0.0f;
		float right_grip = 0.0f;
		bool a = false;
		bool b = false;
		bool x = false;
		bool y = false;
		bool left_stick_click = false;
		bool right_stick_click = false;
		bool menu = false;
	};

	// frame loop (render thread)
	void publish(const controller_state& state);

	// any thread
	controller_state get();

	// Rumble the game asked for (0..1 per hand), already scaled by vr_rumble; zero when the controllers aren't in use
	void rumble(float& left, float& right);

	// main thread, once per user command while head-tracked gameplay is on: degrees to turn (game convention,
	// positive turns left) from the right stick, per vr_turn_mode
	float turn_degrees();

	// main thread: head_tracking tells us whether head-tracked gameplay is on screen; the right stick then turns
	// (and the head does the pitch) instead of being passed to the game
	void set_gameplay_active(bool active);

	std::string status();
}
