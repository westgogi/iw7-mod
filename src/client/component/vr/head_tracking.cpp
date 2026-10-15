#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "head_tracking.hpp"
#include "game_view.hpp"
#include "input.hpp"
#include "vr.hpp"
#include "vr_log.hpp"
#include "xr_math.hpp"

#include "component/scheduler.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

#include <intrin.h>

// Milestone 2: head tracking.
//
// Main thread, once per frame:
//  1. The input code builds the player's command. Our aim hook samples the headset pose (predicted for when this frame
//     will be shown), turns the player by however much the head turned since last frame (plus the right stick's smooth
//     or snap turn) and pulls the aim pitch onto the head's pitch. Aiming, the crosshair and the gun follow your head;
//     the mouse/stick still turn your body.
//  2. Once the game has finished its camera for the frame, it hands it to the renderer (game_view::submit_view; both the
//     single-player and multiplayer frame functions call it). Our hook replaces the camera's orientation with the
//     game's yaw + the full head rotation (so roll and exact pitch are always right), moves it by the head's position
//     offset and widens both of the game's FOV pairs to cover both eyes, then records the pose and FOV the frame is
//     rendered with.
//  3. Before R_EndFrame the record is queued; the render thread pops it when it presents that frame and hands the
//     image to the headset as a projection layer, so the runtime knows exactly where it was rendered from.
// Both eyes still get the same image (no stereo yet).
//
// Yaw: the game's view yaw already contains the head turns we fed into the aim, so the camera simply uses it. If the
// game only shows an aim change a frame (or two) after we feed it, the camera would lag the head by that much and the
// world would swim when you turn your head, so we measure that delay (aim_lag_estimator) and add the head turn the
// game hasn't shown yet.

namespace vr::head_tracking
{
	namespace
	{
		game::dvar_t* vr_mode = nullptr;
		game::dvar_t* vr_head_aim = nullptr;
		game::dvar_t* vr_head_position = nullptr;
		game::dvar_t* vr_world_scale = nullptr;
		game::dvar_t* vr_predict_frames = nullptr;
		game::dvar_t* vr_aim_lag = nullptr;

		constexpr float max_head_offset = 0.6f; // metres; keeps you from leaning far through walls

		utils::hook::detour aim_adjust_hook;
		utils::hook::detour submit_view_hook;
		utils::hook::detour projection_hook;

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
		std::uint64_t frame_number = 1;
		std::uint64_t sampled_frame = 0;

		bool reference_valid = false;
		float reference_yaw = 0.0f; // radians, OpenXR convention
		XrVector3f reference_position{};
		std::uint32_t seen_space_changes = 0;
		std::atomic_bool recenter_requested = false;

		// the head yaw whose turns have been fed into the game's aim so far (same units as pose.yaw)
		bool aim_baseline_valid = false;
		float last_aim_yaw = 0.0f;
		float last_game_pitch = 0.0f; // the game's own view pitch last frame, before our camera override

		std::optional<projection_info> pending_projection;
		std::uint64_t last_camera_frame = 0;
		bool camera_logged = false;
		bool aim_logged = false;
		std::unordered_set<std::string> reported;

		// ---- shared with the render thread / console ------------------------------------------------------------------

		std::mutex records_mutex;
		std::deque<frame_record> records;
		std::atomic<std::uint64_t> view_calls = 0;
		std::atomic<std::uint64_t> camera_frames = 0;
		std::atomic<std::uint64_t> aim_calls = 0;
		std::atomic<float> current_tan_x = 0.0f;
		std::atomic<float> current_tan_y = 0.0f;

		void report_once(const std::string& message)
		{
			if (reported.insert(message).second)
			{
				log::warn("{}", message);
			}
		}

		bool enabled()
		{
			return vr_mode && vr_mode->current.integer == 1;
		}

		bool in_game_recently()
		{
			// the camera hook ran within the last couple of frames
			return last_camera_frame != 0 && last_camera_frame + 2 >= frame_number;
		}

		// ---- aim timing -------------------------------------------------------------------------------------------------

		// Finds how many frames pass between feeding the game a yaw change and the game's view showing it, by
		// comparing the game's frame-to-frame yaw change with the head turn fed 0, 1 and 2 frames earlier.
		// Mouse/stick turns add the same noise to every candidate, so they don't bias the result.
		class aim_lag_estimator
		{
		public:
			static constexpr int max_lag = 2;

			// apply_camera, once per frame: the fed head yaw (last_aim_yaw) and the game's own view yaw
			void record(const std::uint64_t frame, const float fed_yaw, const float game_yaw)
			{
				this->history_[frame % history_size] = {frame, fed_yaw, game_yaw};

				const auto* previous = this->find(frame - 1);
				if (!previous)
				{
					return;
				}

				float fed_turn[max_lag + 1]{};
				auto motion = 0.0f;
				for (auto lag = 0; lag <= max_lag; ++lag)
				{
					const auto* a = this->find(frame - lag);
					const auto* b = this->find(frame - lag - 1);
					if (!a || !b)
					{
						return;
					}

					fed_turn[lag] = math::wrap_degrees(a->fed - b->fed);
					motion += std::abs(fed_turn[lag]);
				}

				const auto game_turn = math::wrap_degrees(game_yaw - previous->game);
				if (motion < 0.3f || std::abs(game_turn) > 30.0f)
				{
					return; // the head is (nearly) still, or the game cut/teleported the view
				}

				for (auto lag = 0; lag <= max_lag; ++lag)
				{
					const auto difference = static_cast<double>(game_turn - fed_turn[lag]);
					this->error_[lag] = this->error_[lag] * 0.97 + difference * difference * 0.03;
				}

				if (++this->samples_ < 90)
				{
					return;
				}

				auto best = 0;
				for (auto lag = 1; lag <= max_lag; ++lag)
				{
					if (this->error_[lag] < this->error_[best])
					{
						best = lag;
					}
				}

				if (!this->reported_)
				{
					this->reported_ = true;
					log::info("Aim timing: the game shows a head turn {} frame(s) after we feed it (fit errors {:.3f} / {:.3f} / {:.3f})",
						best, this->error_[0], this->error_[1], this->error_[2]);
				}

				if (best != this->lag_ && this->error_[best] < 0.5 * this->error_[this->lag_])
				{
					log::info("Aim timing changed: {} -> {} frame(s) (fit errors {:.3f} / {:.3f} / {:.3f})",
						this->lag_, best, this->error_[0], this->error_[1], this->error_[2]);
					this->lag_ = best;
				}
			}

			int lag() const
			{
				if (vr_aim_lag && vr_aim_lag->current.integer >= 0)
				{
					return vr_aim_lag->current.integer;
				}

				return this->lag_;
			}

			// the fed head yaw the game's view shows this frame
			float shown_yaw(const std::uint64_t frame, const float fed_now) const
			{
				const auto lag = this->lag();
				if (lag == 0)
				{
					return fed_now;
				}

				const auto* entry = this->find(frame - lag);
				return entry ? entry->fed : fed_now;
			}

			// a recentre moved the head's relative yaw by `degrees`; move the history with it
			void shift(const float degrees)
			{
				for (auto& entry : this->history_)
				{
					entry.fed += degrees;
				}
			}

			std::string describe() const
			{
				return std::format("{} frame(s){}", this->lag(),
					vr_aim_lag && vr_aim_lag->current.integer >= 0 ? " (vr_aim_lag)" : (this->samples_ < 90 ? " (measuring)" : ""));
			}

		private:
			struct entry
			{
				std::uint64_t frame = 0;
				float fed = 0.0f;
				float game = 0.0f;
			};

			static constexpr std::size_t history_size = 8;
			std::array<entry, history_size> history_{};
			double error_[max_lag + 1]{};
			std::uint64_t samples_ = 0;
			int lag_ = 0;
			bool reported_ = false;

			const entry* find(const std::uint64_t frame) const
			{
				const auto& candidate = this->history_[frame % history_size];
				return candidate.frame == frame && frame != 0 ? &candidate : nullptr;
			}
		};

		aim_lag_estimator aim_lag{};

		// pitch corrections fed in recent frames, so a delayed game doesn't get the same correction twice
		struct pitch_feed
		{
			std::uint64_t frame = 0;
			float amount = 0.0f;
		};

		std::array<pitch_feed, 4> pitch_feeds{};

		float pending_pitch()
		{
			auto pending = 0.0f;
			for (auto i = 1; i <= aim_lag.lag(); ++i)
			{
				const auto& feed = pitch_feeds[(frame_number - i) % pitch_feeds.size()];
				if (feed.frame == frame_number - i)
				{
					pending += feed.amount;
				}
			}

			return pending;
		}

		void add_pitch_feed(const float amount)
		{
			auto& feed = pitch_feeds[frame_number % pitch_feeds.size()];
			if (feed.frame != frame_number)
			{
				feed = {frame_number, 0.0f};
			}

			feed.amount += amount;
		}

		// Samples the headset once per game frame
		bool sample_pose()
		{
			if (sampled_frame == frame_number)
			{
				return pose.valid;
			}

			sampled_frame = frame_number;
			const auto previous_yaw = pose.yaw;
			const auto had_pose = reference_valid;
			pose.valid = false;

			view_state views{};
			const auto frames_ahead = vr_predict_frames ? vr_predict_frames->current.value : 1.0f;
			if (!locate_predicted(frames_ahead, views))
			{
				report_once("Head tracking: couldn't get the headset pose this frame (VR not running yet?)");
				return false;
			}

			const auto space_changes = space_change_count();
			const auto recenter = recenter_requested.exchange(false);
			const auto rebaseline = space_changes != seen_space_changes || recenter;
			if (rebaseline)
			{
				seen_space_changes = space_changes;
				reference_valid = false;
			}

			if (!reference_valid)
			{
				reference_yaw = math::yaw_of(views.head.orientation);
				reference_position = views.head.position;
				reference_valid = true;
				log::info("Head tracking reference set");
			}

			const auto unyaw = math::yaw_rotation(-reference_yaw);
			pose.relative_orientation = math::multiply(unyaw, views.head.orientation);
			pose.relative_position = math::rotate(unyaw, math::subtract(views.head.position, reference_position));
			math::game_yaw_pitch(pose.relative_orientation, pose.yaw, pose.pitch);
			pose.views = views;
			pose.valid = true;

			if (rebaseline && had_pose)
			{
				// The head's yaw relative to the reference just jumped. Move the fed-yaw bookkeeping by the same amount so
				// neither the view nor the aim jumps; only the head position offset starts again from zero.
				const auto jump = math::wrap_degrees(pose.yaw - previous_yaw);
				last_aim_yaw += jump;
				aim_lag.shift(jump);
			}

			return true;
		}

		std::uint64_t aim_adjust_stub(std::uint8_t* input, float* output)
		{
			const auto result = aim_adjust_hook.invoke<std::uint64_t>(input, output);
			++aim_calls;

			const auto local_client = *reinterpret_cast<int*>(input + game_view::aim_input_local_client);
			if (local_client != 0 || !enabled() || !in_game_recently() || !sample_pose())
			{
				input::turn_degrees(); // keeps its clock current, so the first turn after a pause isn't a jump
				return result;
			}

			// right stick: smooth or snap turn
			output[1] += input::turn_degrees();

			if (!vr_head_aim->current.enabled)
			{
				return result;
			}

			// yaw: add how far the head turned since last time, so the mouse/stick can still turn the body. After a
			// pause (menu, cutscene) this feeds everything the head turned meanwhile, which brings the aim back in line
			// with the view.
			if (aim_baseline_valid)
			{
				output[1] += math::wrap_degrees(pose.yaw - last_aim_yaw);
			}

			last_aim_yaw = pose.yaw;
			aim_baseline_valid = true;

			// pitch: pull the player's view pitch onto the head's pitch (corrects whatever the mouse/stick did)
			const auto correction = math::wrap_degrees(pose.pitch - (last_game_pitch + pending_pitch()));
			output[0] += correction;
			add_pitch_feed(correction);

			if (!aim_logged)
			{
				aim_logged = true;
				log::info("Head aim active on thread {} (head yaw {:.1f}, pitch {:.1f})", GetCurrentThreadId(), pose.yaw, pose.pitch);
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

		void apply_camera(std::uint8_t* refdef)
		{
			auto* view = game_view::view(refdef);

			// the game's own camera direction this frame
			const math::vec3 game_forward{view->axis[0][0], view->axis[0][1], view->axis[0][2]};
			const auto game_yaw = std::atan2(game_forward.y, game_forward.x) * math::to_degrees;
			last_game_pitch = -std::asin(std::clamp(game_forward.z, -1.0f, 1.0f)) * math::to_degrees;

			// the camera faces the game's yaw plus whatever head turn the game's view doesn't show yet
			aim_lag.record(frame_number, last_aim_yaw, game_yaw);
			const auto camera_yaw = game_yaw + math::wrap_degrees(pose.yaw - aim_lag.shown_yaw(frame_number, last_aim_yaw));

			// the head's rotation is applied relative to this heading
			const auto body_yaw = camera_yaw - pose.yaw;

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

			// both pairs: the first drives culling and LOD, the second is what the scene is actually projected with
			const auto game_tan_x = view->tan_half_fov_x;
			const auto game_tan_y = view->tan_half_fov_y;
			const auto game_draw_tan_x = view->draw_tan_half_fov[0];
			const auto game_draw_tan_y = view->draw_tan_half_fov[1];
			view->tan_half_fov_x = tan_x;
			view->tan_half_fov_y = tan_y;
			view->draw_tan_half_fov[0] = tan_x;
			view->draw_tan_half_fov[1] = tan_y;
			current_tan_x = tan_x;
			current_tan_y = tan_y;

			std::memcpy(game_view::effects_origin(refdef), view->origin, sizeof(float) * 3);
			axis_to_angles(axis, game_view::effects_angles(refdef));

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
				log::info("Head-tracked camera active on thread {}: game yaw {:.1f} pitch {:.1f}, head yaw {:.1f} pitch {:.1f}, "
					"fov tan {:.3f} x {:.3f} (the game had {:.3f} x {:.3f}, drawing with {:.3f} x {:.3f})",
					GetCurrentThreadId(), game_yaw, last_game_pitch, pose.yaw, pose.pitch, tan_x, tan_y,
					game_tan_x, game_tan_y, game_draw_tan_x, game_draw_tan_y);
			}
		}

		void submit_view_stub(const int local_client, std::uint8_t* refdef, std::uint8_t* view, const float z_near,
			const int a5, const int a6, const char skip)
		{
			++view_calls;

			if (local_client == 0 && !skip && enabled() && refdef && sample_pose())
			{
				last_camera_frame = frame_number;
				++camera_frames;
				apply_camera(refdef);
			}

			submit_view_hook.invoke<void>(local_client, refdef, view, z_near, a5, a6, skip);
		}

		// ---- projection probe: which FOVs the renderer really projects with -------------------------------------------
		// Any thread (render workers). Collects distinct (caller, tanX, tanY) combinations; the log lists new ones.

		struct probe_entry
		{
			std::uintptr_t caller = 0;
			float tan_x = 0.0f;
			float tan_y = 0.0f;
			std::uint64_t calls = 0;
			std::uint64_t reported_calls = 0;
			bool logged = false;
		};

		std::mutex probe_mutex;
		std::array<probe_entry, 48> probe_entries{};
		std::size_t probe_count = 0;
		std::atomic<std::uint64_t> probe_overflow = 0;

		void projection_matrix_stub(const float tan_x, const float tan_y, const float z_near, float* out)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

			{
				std::lock_guard _(probe_mutex);
				auto found = false;
				for (std::size_t i = 0; i < probe_count; ++i)
				{
					auto& entry = probe_entries[i];
					if (entry.caller == caller && std::abs(entry.tan_x - tan_x) < 0.002f && std::abs(entry.tan_y - tan_y) < 0.002f)
					{
						++entry.calls;
						found = true;
						break;
					}
				}

				if (!found)
				{
					if (probe_count < probe_entries.size())
					{
						probe_entries[probe_count++] = {caller, tan_x, tan_y, 1, 0, false};
					}
					else
					{
						++probe_overflow;
					}
				}
			}

			projection_hook.invoke<void>(tan_x, tan_y, z_near, out);
		}

		// async, every few seconds: log combinations seen for the first time
		void log_projection_probe()
		{
			std::vector<std::string> lines;
			{
				std::lock_guard _(probe_mutex);
				for (std::size_t i = 0; i < probe_count; ++i)
				{
					auto& entry = probe_entries[i];
					if (!entry.logged)
					{
						entry.logged = true;
						lines.push_back(std::format("Renderer projection from {:#x}: tan {:.3f} x {:.3f} ({:.1f} x {:.1f} degrees full FOV)",
							entry.caller, entry.tan_x, entry.tan_y,
							2.0f * std::atan(entry.tan_x) * math::to_degrees, 2.0f * std::atan(entry.tan_y) * math::to_degrees));
					}
				}
			}

			for (const auto& line : lines)
			{
				log::info("{} (VR camera now {:.3f} x {:.3f})", line, current_tan_x.load(), current_tan_y.load());
			}
		}

		// async, every minute: how often each one was used
		void summarize_projection_probe()
		{
			std::string summary;
			{
				std::lock_guard _(probe_mutex);
				for (std::size_t i = 0; i < probe_count; ++i)
				{
					auto& entry = probe_entries[i];
					const auto calls = entry.calls - entry.reported_calls;
					entry.reported_calls = entry.calls;
					if (calls > 0)
					{
						summary += std::format("{:#x} {:.3f}x{:.3f}: {}, ", entry.caller, entry.tan_x, entry.tan_y, calls);
					}
				}
			}

			if (!summary.empty())
			{
				summary.resize(summary.size() - 2);
				log::info("Renderer projections in the last minute: {}", summary);
			}
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

			input::set_gameplay_active(enabled() && in_game_recently());
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
		return std::format("mode: {} | head aim: {} | head position: {} | camera calls: {} | head-tracked frames: {} | aim calls: {} | aim timing: {}",
			enabled() ? "head tracking" : "screen only",
			vr_head_aim && vr_head_aim->current.enabled ? "on" : "off",
			vr_head_position && vr_head_position->current.enabled ? "on" : "off",
			view_calls.load(), camera_frames.load(), aim_calls.load(), aim_lag.describe());
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
			vr_aim_lag = game::Dvar_RegisterInt("vr_aim_lag", -1, -1, 2, game::DVAR_FLAG_NONE,
				"VR: frames between feeding the game a head turn and its view showing it. -1 = measure automatically");

			aim_adjust_hook.create(game_view::aim_adjust, reinterpret_cast<void*>(aim_adjust_stub));
			submit_view_hook.create(game_view::submit_view, reinterpret_cast<void*>(submit_view_stub));
			projection_hook.create(game_view::projection_matrix, reinterpret_cast<void*>(projection_matrix_stub));

			scheduler::loop(end_frame, scheduler::renderer);

			// every 10 s, note in the log if the game is drawing views but none could be head-tracked
			scheduler::loop([]
			{
				static std::uint64_t last_views = 0;
				static std::uint64_t last_tracked = 0;
				const auto views = view_calls.load();
				const auto tracked = camera_frames.load();
				if (views != last_views && tracked == last_tracked && enabled())
				{
					log::warn("Head tracking: the game drew {} views in the last 10 s but none were head-tracked", views - last_views);
				}

				last_views = views;
				last_tracked = tracked;
			}, scheduler::async, 10s);

			scheduler::loop(log_projection_probe, scheduler::async, 3s);
			scheduler::loop(summarize_projection_probe, scheduler::async, 60s);
		}
	};
}

REGISTER_COMPONENT(vr::head_tracking::component)
