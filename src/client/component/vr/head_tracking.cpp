#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "head_tracking.hpp"
#include "game_view.hpp"
#include "vr.hpp"
#include "vr_log.hpp"
#include "xr_math.hpp"

#include "component/scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

// Milestone 2: head tracking.
//
// Main thread, once per frame:
//  1. The input code builds the player's command. Our aim hook samples the headset pose (predicted for when this frame
//     will be shown) and turns the player by however much the head turned since last frame, and sets the aim pitch to
//     the head's pitch. Aiming, the crosshair and the gun follow your head; the mouse/stick still turn your body.
//  2. CG_CalcViewValues builds the camera. Our camera hook then replaces its orientation with body yaw + the full head
//     rotation (so roll and exact pitch are always right), moves it by the head's position offset, and widens the
//     field of view to cover both eyes. It records the pose and FOV this frame was rendered with.
//  3. Before R_EndFrame the record is queued; the render thread pops it when it presents that frame and hands the
//     image to the headset as a projection layer, so the runtime knows exactly where it was rendered from.
// Both eyes still get the same image (no stereo yet).

namespace vr::head_tracking
{
	namespace
	{
		game::dvar_t* vr_mode = nullptr;
		game::dvar_t* vr_head_aim = nullptr;
		game::dvar_t* vr_head_position = nullptr;
		game::dvar_t* vr_world_scale = nullptr;
		game::dvar_t* vr_predict_frames = nullptr;

		constexpr float max_head_offset = 0.6f; // metres; keeps you from leaning far through walls

		utils::hook::detour aim_adjust_hook;
		utils::hook::detour cg_calc_view_values_hook;

		// ---- main thread state ----------------------------------------------------------------------------------------

		struct sampled_pose
		{
			bool valid = false;
			view_state views{};
			XrQuaternionf relative_orientation{0.0f, 0.0f, 0.0f, 1.0f}; // head rotation relative to the reference heading
			XrVector3f relative_position{};                              // metres from the reference point
			float yaw = 0.0f;                                            // game-convention degrees, relative
			float pitch = 0.0f;
		};

		sampled_pose pose{};
		std::uint64_t frame_number = 0;
		std::uint64_t sampled_frame = ~0ull;

		bool reference_valid = false;
		float reference_yaw = 0.0f; // radians, OpenXR convention
		XrVector3f reference_position{};
		std::uint32_t seen_space_changes = 0;
		std::atomic_bool recenter_requested = false;

		bool aim_baseline_valid = false;
		float last_aim_yaw = 0.0f;

		std::optional<projection_info> pending_projection;
		std::uint64_t last_camera_frame = 0;
		bool camera_logged = false;
		bool aim_logged = false;

		// ---- shared with the render thread --------------------------------------------------------------------------

		std::mutex records_mutex;
		std::deque<frame_record> records;
		std::atomic<std::uint64_t> camera_frames = 0;

		bool enabled()
		{
			return vr_mode && vr_mode->current.integer == 1;
		}

		bool in_game_recently()
		{
			// the camera hook ran within the last couple of frames
			return last_camera_frame + 2 >= frame_number && last_camera_frame != 0;
		}

		// Samples the headset once per game frame
		bool sample_pose()
		{
			if (sampled_frame == frame_number)
			{
				return pose.valid;
			}

			sampled_frame = frame_number;
			pose.valid = false;

			view_state views{};
			const auto frames_ahead = vr_predict_frames ? vr_predict_frames->current.value : 1.0f;
			if (!locate_predicted(frames_ahead, views))
			{
				return false;
			}

			const auto space_changes = space_change_count();
			if (space_changes != seen_space_changes || recenter_requested.exchange(false))
			{
				seen_space_changes = space_changes;
				reference_valid = false;
			}

			if (!reference_valid)
			{
				reference_yaw = math::yaw_of(views.head.orientation);
				reference_position = views.head.position;
				reference_valid = true;
				aim_baseline_valid = false;
				log::info("Head tracking reference set");
			}

			const auto unyaw = math::yaw_rotation(-reference_yaw);
			pose.relative_orientation = math::multiply(unyaw, views.head.orientation);
			pose.relative_position = math::rotate(unyaw, math::subtract(views.head.position, reference_position));
			math::game_yaw_pitch(pose.relative_orientation, pose.yaw, pose.pitch);
			pose.views = views;
			pose.valid = true;
			return true;
		}

		std::uint64_t aim_adjust_stub(std::uint8_t* input, float* output)
		{
			const auto result = aim_adjust_hook.invoke<std::uint64_t>(input, output);

			const auto local_client = *reinterpret_cast<int*>(input + game_view::aim_input_local_client);
			if (local_client != 0 || !enabled() || !vr_head_aim->current.enabled || !in_game_recently() || !sample_pose())
			{
				aim_baseline_valid = false;
				return result;
			}

			// yaw: add how far the head turned since last frame, so the mouse/stick can still turn the body
			if (aim_baseline_valid)
			{
				output[1] += math::wrap_degrees(pose.yaw - last_aim_yaw);
			}

			last_aim_yaw = pose.yaw;
			aim_baseline_valid = true;

			// pitch: the player's view pitch is command pitch + delta_angles, so this puts it exactly on the head's pitch
			if (auto* cg = game_view::cg(0))
			{
				output[0] = math::wrap_degrees(pose.pitch - game_view::player_delta_angles(cg)[0]);
			}

			if (!aim_logged)
			{
				aim_logged = true;
				log::info("Head aim active (head yaw {:.1f}, pitch {:.1f})", pose.yaw, pose.pitch);
			}

			return result;
		}

		void axis_to_angles(const math::vec3 (&axis)[3], float* angles)
		{
			const auto& forward = axis[0];
			angles[0] = -std::asin(std::clamp(forward.z, -1.0f, 1.0f)) * math::to_degrees;
			angles[1] = std::atan2(forward.y, forward.x) * math::to_degrees;
			// roll: how far the left axis tilts up around the forward axis
			const math::vec3 flat_left{-forward.y, forward.x, 0.0f};
			const auto flat_length = std::sqrt(flat_left.x * flat_left.x + flat_left.y * flat_left.y);
			if (flat_length > 1e-4f)
			{
				const auto cos_roll = (axis[1].x * flat_left.x + axis[1].y * flat_left.y) / flat_length;
				angles[2] = std::atan2(axis[1].z, cos_roll) * math::to_degrees;
			}
			else
			{
				angles[2] = 0.0f;
			}
		}

		void apply_camera(std::uint8_t* cg)
		{
			auto* view = game_view::view(cg);
			const auto* game_angles = game_view::view_angles(cg);

			// where the body faces in the game: the game's view yaw minus where the head looks
			const auto body_yaw = game_angles[1] - pose.yaw;

			math::vec3 axis[3];
			math::to_game_axis(pose.relative_orientation, axis);
			for (auto i = 0; i < 3; ++i)
			{
				axis[i] = math::rotate_yaw(axis[i], body_yaw);
				view->axis[i][0] = axis[i].x;
				view->axis[i][1] = axis[i].y;
				view->axis[i][2] = axis[i].z;
			}

			if (vr_head_position->current.enabled)
			{
				auto offset = pose.relative_position;
				const auto length = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
				if (length > max_head_offset)
				{
					offset = math::scale(offset, max_head_offset / length);
				}

				const auto moved = math::rotate_yaw(math::to_game(math::scale(offset, vr_world_scale->current.value)), body_yaw);
				view->origin[0] += moved.x;
				view->origin[1] += moved.y;
				view->origin[2] += moved.z;
			}

			// one symmetric frustum that covers both eyes; the runtime is told exactly this FOV
			auto tan_x = 0.0f;
			auto tan_y = 0.0f;
			for (const auto& fov : pose.views.eye_fov)
			{
				tan_x = std::max({tan_x, std::tan(-fov.angleLeft), std::tan(fov.angleRight)});
				tan_y = std::max({tan_y, std::tan(fov.angleUp), std::tan(-fov.angleDown)});
			}

			view->tan_half_fov_x = tan_x;
			view->tan_half_fov_y = tan_y;

			auto* effects_origin = game_view::effects_origin(cg);
			std::memcpy(effects_origin, view->origin, sizeof(float) * 3);
			axis_to_angles(axis, game_view::effects_angles(cg));

			projection_info projection{};
			projection.pose = pose.views.head;
			projection.fov.angleLeft = -std::atan(tan_x);
			projection.fov.angleRight = std::atan(tan_x);
			projection.fov.angleUp = std::atan(tan_y);
			projection.fov.angleDown = -std::atan(tan_y);
			pending_projection = projection;

			if (!camera_logged)
			{
				camera_logged = true;
				log::info("Head-tracked camera active on thread {}: body yaw {:.1f}, head yaw {:.1f} pitch {:.1f}, fov tan {:.3f} x {:.3f}",
					GetCurrentThreadId(), body_yaw, pose.yaw, pose.pitch, tan_x, tan_y);
			}
		}

		std::uint64_t cg_calc_view_values_stub(const int local_client)
		{
			const auto result = cg_calc_view_values_hook.invoke<std::uint64_t>(local_client);

			if (local_client != 0 || !enabled())
			{
				return result;
			}

			auto* cg = game_view::cg(0);
			if (!cg || !sample_pose())
			{
				return result;
			}

			last_camera_frame = frame_number;
			++camera_frames;
			apply_camera(cg);
			return result;
		}

		// main thread, right before R_EndFrame: queue what this frame was rendered with
		void end_frame()
		{
			frame_record record{};
			if (pending_projection)
			{
				record.head_tracked = true;
				record.projection = *pending_projection;
				pending_projection.reset();
			}

			{
				std::lock_guard _(records_mutex);
				records.push_back(record);
				while (records.size() > 3)
				{
					records.pop_front(); // the render thread fell behind or skipped presents; drop the oldest
				}
			}

			++frame_number;
		}
	}

	frame_record pop_frame_record()
	{
		std::lock_guard _(records_mutex);
		if (records.empty())
		{
			return {};
		}

		const auto record = records.front();
		records.pop_front();

		if (!enabled())
		{
			return {};
		}

		return record;
	}

	void recenter()
	{
		recenter_requested = true;
	}

	std::string status()
	{
		return std::format("mode: {} | head aim: {} | head position: {} | head-tracked game frames: {}",
			enabled() ? "head tracking" : "screen only",
			vr_head_aim && vr_head_aim->current.enabled ? "on" : "off",
			vr_head_position && vr_head_position->current.enabled ? "on" : "off",
			camera_frames.load());
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			vr_mode = game::Dvar_RegisterInt("vr_mode", 1, 0, 1, game::DVAR_FLAG_SAVED,
				"VR: 0 = always show the game on a virtual screen, 1 = head tracking in game (menus stay on the screen)");
			vr_head_aim = game::Dvar_RegisterBool("vr_head_aim", true, game::DVAR_FLAG_SAVED,
				"VR: aim where you look; the mouse/stick still turn your body");
			vr_head_position = game::Dvar_RegisterBool("vr_head_position", true, game::DVAR_FLAG_SAVED,
				"VR: move the camera with your head (leaning, crouching)");
			vr_world_scale = game::Dvar_RegisterFloat("vr_world_scale", 39.37f, 10.0f, 100.0f, game::DVAR_FLAG_SAVED,
				"VR: game units per metre of head movement");
			vr_predict_frames = game::Dvar_RegisterFloat("vr_predict_frames", 1.0f, 0.0f, 4.0f, game::DVAR_FLAG_SAVED,
				"VR: how many frames ahead to predict the head pose");

			aim_adjust_hook.create(game_view::aim_adjust, aim_adjust_stub);
			cg_calc_view_values_hook.create(game_view::cg_calc_view_values, cg_calc_view_values_stub);

			scheduler::loop(end_frame, scheduler::renderer);
		}
	};
}

REGISTER_COMPONENT(vr::head_tracking::component)
