#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "input.hpp"
#include "game_view.hpp"
#include "vr_log.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

#include <Xinput.h>

// Touch controllers -> virtual Xbox controller.
//
// The game reads gamepads through XINPUT9_1_0.dll (XInputGetState / XInputSetState / XInputGetCapabilities). Our
// load_import() hands it these stubs instead, which report the Touch controllers as controller 0 (merged with a real
// pad on that slot, if there is one). The default Xbox layout lines up with the Touch controllers:
//   left stick: move                 right stick: turn (smooth/snap, see vr_turn_mode)
//   left trigger: aim down sights    right trigger: fire
//   left grip: tactical (LB)         right grip: lethal (RB)
//   A: jump    B: crouch / prone     X: reload / use    Y: switch weapon
//   left stick click: sprint         right stick click: melee
//   left menu button: pause (Start)
// Rumble from the game is played on the controller of the matching side.

namespace vr::input
{
	namespace
	{
		game::dvar_t* vr_controllers = nullptr;
		game::dvar_t* vr_turn_mode = nullptr;
		game::dvar_t* vr_turn_speed = nullptr;
		game::dvar_t* vr_snap_angle = nullptr;
		game::dvar_t* vr_stick_pitch = nullptr;
		game::dvar_t* vr_rumble = nullptr;

		std::mutex state_mutex;
		controller_state latest{};
		std::uint64_t publish_count = 0;

		std::atomic_bool gameplay_active = false;
		std::atomic<float> rumble_left = 0.0f;
		std::atomic<float> rumble_right = 0.0f;

		std::atomic<std::uint64_t> get_state_calls = 0;
		std::atomic<std::uint64_t> virtual_reports = 0;
		std::atomic_bool first_report_logged = false;

		utils::hook::detour gamepad_enabled_hook;

		using xinput_get_state_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		using xinput_set_state_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
		using xinput_get_capabilities_t = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

		xinput_get_state_t real_get_state = nullptr;
		xinput_set_state_t real_set_state = nullptr;
		xinput_get_capabilities_t real_get_capabilities = nullptr;

		void load_real_xinput()
		{
			static std::once_flag once;
			std::call_once(once, []
			{
				auto* module = LoadLibraryA("XINPUT9_1_0.dll");
				if (!module)
				{
					module = LoadLibraryA("xinput1_4.dll");
				}

				if (module)
				{
					real_get_state = reinterpret_cast<xinput_get_state_t>(GetProcAddress(module, "XInputGetState"));
					real_set_state = reinterpret_cast<xinput_set_state_t>(GetProcAddress(module, "XInputSetState"));
					real_get_capabilities = reinterpret_cast<xinput_get_capabilities_t>(GetProcAddress(module, "XInputGetCapabilities"));
				}
			});
		}

		bool controllers_enabled()
		{
			return !vr_controllers || vr_controllers->current.enabled;
		}

		// the Touch controllers are what the game should see as controller 0 right now
		bool virtual_pad_active()
		{
			if (!controllers_enabled())
			{
				return false;
			}

			std::lock_guard _(state_mutex);
			return latest.connected;
		}

		int turn_mode()
		{
			return vr_turn_mode ? vr_turn_mode->current.integer : 1;
		}

		SHORT to_axis(const float value)
		{
			const auto clamped = std::clamp(value, -1.0f, 1.0f);
			return static_cast<SHORT>(clamped >= 0.0f ? clamped * 32767.0f : clamped * 32768.0f);
		}

		BYTE to_trigger(const float value)
		{
			return static_cast<BYTE>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
		}

		SHORT stronger(const SHORT a, const SHORT b)
		{
			return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
		}

		// grips are analog; press at 60 %, release at 40 % so a resting hand doesn't flicker the button
		bool grip_pressed(const float value, bool& latch)
		{
			if (latch)
			{
				latch = value > 0.4f;
			}
			else
			{
				latch = value > 0.6f;
			}

			return latch;
		}

		struct pad_merge_state
		{
			bool left_grip_latch = false;
			bool right_grip_latch = false;
			DWORD packet = 1;
			XINPUT_GAMEPAD last{};
		};

		pad_merge_state merge_state{};

		DWORD WINAPI get_state_stub(const DWORD user_index, XINPUT_STATE* state)
		{
			load_real_xinput();
			++get_state_calls;

			auto result = real_get_state ? real_get_state(user_index, state) : static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED);
			if (user_index != 0 || !state || !controllers_enabled())
			{
				return result;
			}

			// While the headset menu has focus (or the controllers are idle) the pad stays connected but neutral, so the
			// game doesn't pause for a "disconnected" controller every time the Meta menu opens
			const auto touch = get();
			if (!touch.connected)
			{
				return result;
			}

			if (result != ERROR_SUCCESS)
			{
				std::memset(state, 0, sizeof(*state));
			}

			std::lock_guard _(state_mutex);

			auto& pad = state->Gamepad;
			WORD buttons = 0;
			if (touch.a) buttons |= XINPUT_GAMEPAD_A;
			if (touch.b) buttons |= XINPUT_GAMEPAD_B;
			if (touch.x) buttons |= XINPUT_GAMEPAD_X;
			if (touch.y) buttons |= XINPUT_GAMEPAD_Y;
			if (touch.left_stick_click) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
			if (touch.right_stick_click) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
			if (touch.menu) buttons |= XINPUT_GAMEPAD_START;
			if (grip_pressed(touch.left_grip, merge_state.left_grip_latch)) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
			if (grip_pressed(touch.right_grip, merge_state.right_grip_latch)) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
			pad.wButtons |= buttons;

			auto right_x = touch.right_stick[0];
			auto right_y = touch.right_stick[1];
			if (gameplay_active)
			{
				// in head-tracked gameplay we turn with the stick ourselves and the head does the pitch
				if (turn_mode() != 0)
				{
					right_x = 0.0f;
				}

				if (!vr_stick_pitch || !vr_stick_pitch->current.enabled)
				{
					right_y = 0.0f;
				}
			}

			pad.sThumbLX = stronger(pad.sThumbLX, to_axis(touch.left_stick[0]));
			pad.sThumbLY = stronger(pad.sThumbLY, to_axis(touch.left_stick[1]));
			pad.sThumbRX = stronger(pad.sThumbRX, to_axis(right_x));
			pad.sThumbRY = stronger(pad.sThumbRY, to_axis(right_y));
			pad.bLeftTrigger = std::max(pad.bLeftTrigger, to_trigger(touch.left_trigger));
			pad.bRightTrigger = std::max(pad.bRightTrigger, to_trigger(touch.right_trigger));

			// games skip a state whose packet number didn't change
			if (std::memcmp(&pad, &merge_state.last, sizeof(pad)) != 0)
			{
				merge_state.last = pad;
				++merge_state.packet;
			}

			state->dwPacketNumber = merge_state.packet;
			++virtual_reports;

			if (!first_report_logged.exchange(true))
			{
				log::info("Touch controllers are now reported to the game as controller 1 (thread {})", GetCurrentThreadId());
			}

			return ERROR_SUCCESS;
		}

		DWORD WINAPI set_state_stub(const DWORD user_index, XINPUT_VIBRATION* vibration)
		{
			load_real_xinput();

			if (user_index == 0 && vibration)
			{
				rumble_left = static_cast<float>(vibration->wLeftMotorSpeed) / 65535.0f;
				rumble_right = static_cast<float>(vibration->wRightMotorSpeed) / 65535.0f;
			}

			const auto result = real_set_state ? real_set_state(user_index, vibration) : static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED);
			if (result != ERROR_SUCCESS && user_index == 0 && virtual_pad_active())
			{
				return ERROR_SUCCESS;
			}

			return result;
		}

		DWORD WINAPI get_capabilities_stub(const DWORD user_index, const DWORD flags, XINPUT_CAPABILITIES* capabilities)
		{
			load_real_xinput();

			const auto result = real_get_capabilities
				? real_get_capabilities(user_index, flags, capabilities)
				: static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED);

			if (result == ERROR_SUCCESS || user_index != 0 || !capabilities || !virtual_pad_active())
			{
				return result;
			}

			std::memset(capabilities, 0, sizeof(*capabilities));
			capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
			capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
			capabilities->Gamepad.wButtons = 0xF3FF;
			capabilities->Gamepad.bLeftTrigger = 0xFF;
			capabilities->Gamepad.bRightTrigger = 0xFF;
			capabilities->Gamepad.sThumbLX = static_cast<SHORT>(0xFFC0);
			capabilities->Gamepad.sThumbLY = static_cast<SHORT>(0xFFC0);
			capabilities->Gamepad.sThumbRX = static_cast<SHORT>(0xFFC0);
			capabilities->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
			capabilities->Vibration.wLeftMotorSpeed = 0xFFFF;
			capabilities->Vibration.wRightMotorSpeed = 0xFFFF;
			return ERROR_SUCCESS;
		}

		// The PC profile can switch the gamepad off ("gpadEnabled"); while the Touch controllers are in use, treat it as on
		std::uint32_t gamepad_enabled_stub(const int controller_index)
		{
			if (virtual_pad_active())
			{
				return 1;
			}

			return gamepad_enabled_hook.invoke<std::uint32_t>(controller_index);
		}
	}

	void publish(const controller_state& state)
	{
		std::lock_guard _(state_mutex);
		latest = state;
		++publish_count;
	}

	controller_state get()
	{
		std::lock_guard _(state_mutex);
		return latest;
	}

	void rumble(float& left, float& right)
	{
		left = 0.0f;
		right = 0.0f;

		if (!virtual_pad_active())
		{
			return;
		}

		const auto strength = vr_rumble ? vr_rumble->current.value : 0.6f;
		left = rumble_left * strength;
		right = rumble_right * strength;
	}

	float turn_degrees()
	{
		using clock = std::chrono::steady_clock;
		static auto last_call = clock::now();
		static auto snap_latched = false;

		const auto now = clock::now();
		const auto dt = std::clamp(std::chrono::duration<float>(now - last_call).count(), 0.0f, 0.05f);
		last_call = now;

		const auto mode = turn_mode();
		if (mode == 0 || !controllers_enabled())
		{
			snap_latched = false;
			return 0.0f;
		}

		const auto state = get();
		if (!state.active)
		{
			snap_latched = false;
			return 0.0f;
		}

		const auto x = state.right_stick[0];

		if (mode == 2)
		{
			// snap turn: one step per push, the stick has to come back towards the middle before the next one
			if (!snap_latched && std::abs(x) > 0.7f)
			{
				snap_latched = true;
				const auto angle = vr_snap_angle ? vr_snap_angle->current.value : 45.0f;
				return x > 0.0f ? -angle : angle;
			}

			if (snap_latched && std::abs(x) < 0.35f)
			{
				snap_latched = false;
			}

			return 0.0f;
		}

		// smooth turn: proportional to how far the stick is pushed past a small dead zone
		constexpr auto dead_zone = 0.15f;
		const auto magnitude = std::abs(x);
		if (magnitude < dead_zone)
		{
			return 0.0f;
		}

		const auto amount = (magnitude - dead_zone) / (1.0f - dead_zone);
		const auto speed = vr_turn_speed ? vr_turn_speed->current.value : 135.0f;
		return (x > 0.0f ? -amount : amount) * speed * dt;
	}

	void set_gameplay_active(const bool active)
	{
		gameplay_active = active;
	}

	std::string status()
	{
		const auto state = get();
		return std::format("controllers: {} | game polls: {} | reported as pad: {} | turn mode: {}",
			!controllers_enabled() ? "off (vr_controllers 0)" : (state.active ? "active" : (state.connected ? "idle" : "not in use")),
			get_state_calls.load(), virtual_reports.load(),
			turn_mode() == 0 ? "game stick" : (turn_mode() == 1 ? "smooth" : "snap"));
	}

	class component final : public component_interface
	{
	public:
		void* load_import(const std::string& library, const std::string& function) override
		{
			if (_stricmp(library.data(), "XINPUT9_1_0.dll") != 0)
			{
				return nullptr;
			}

			if (function == "XInputGetState")
			{
				return reinterpret_cast<void*>(get_state_stub);
			}

			if (function == "XInputSetState")
			{
				return reinterpret_cast<void*>(set_state_stub);
			}

			if (function == "XInputGetCapabilities")
			{
				return reinterpret_cast<void*>(get_capabilities_stub);
			}

			return nullptr;
		}

		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			vr_controllers = game::Dvar_RegisterBool("vr_controllers", true, game::DVAR_FLAG_SAVED,
				"VR: use the VR controllers as a gamepad");
			vr_turn_mode = game::Dvar_RegisterInt("vr_turn_mode", 1, 0, 2, game::DVAR_FLAG_SAVED,
				"VR: right stick turning. 0 = the game's own stick look, 1 = smooth turn, 2 = snap turn");
			vr_turn_speed = game::Dvar_RegisterFloat("vr_turn_speed", 135.0f, 10.0f, 720.0f, game::DVAR_FLAG_SAVED,
				"VR: smooth turn speed at full stick, degrees per second");
			vr_snap_angle = game::Dvar_RegisterFloat("vr_snap_angle", 45.0f, 5.0f, 180.0f, game::DVAR_FLAG_SAVED,
				"VR: snap turn step, degrees");
			vr_stick_pitch = game::Dvar_RegisterBool("vr_stick_pitch", false, game::DVAR_FLAG_SAVED,
				"VR: let the right stick also look up and down in game (normally your head does that)");
			vr_rumble = game::Dvar_RegisterFloat("vr_rumble", 0.6f, 0.0f, 1.0f, game::DVAR_FLAG_SAVED,
				"VR: controller vibration strength, 0 = off");

			gamepad_enabled_hook.create(game_view::gamepad_enabled, reinterpret_cast<void*>(gamepad_enabled_stub));
		}
	};
}

REGISTER_COMPONENT(vr::input::component)
