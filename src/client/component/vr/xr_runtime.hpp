#pragma once

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif

#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace vr
{
	// Thrown when VR can't start. retry() is true for conditions that usually clear up on their own
	// (runtime still starting, Link not connected yet) and false when the user has to change something.
	class xr_error : public std::runtime_error
	{
	public:
		xr_error(const std::string& message, const bool retry)
			: std::runtime_error(message), retry_(retry)
		{
		}

		bool retry() const
		{
			return retry_;
		}

	private:
		bool retry_;
	};

	struct screen_params
	{
		float distance = 2.5f;      // metres in front of the recentred head position
		float width = 3.0f;         // metres; the height follows the game's aspect ratio
		float height_offset = 0.0f; // metres, positive moves the screen up
		bool follow_head = false;   // lock the screen to the headset instead of the room
		bool srgb = true;           // treat the game's image as gamma (sRGB) encoded
	};

	// Where the headset and both eyes are at a given time, in the LOCAL reference space
	struct view_state
	{
		XrTime time = 0;
		XrPosef head{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		XrPosef eye_pose[2]{};
		XrFovf eye_fov[2]{};
	};

	// What a game frame was rendered with, so the headset can show it from the right place
	struct projection_info
	{
		XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // LOCAL space
		XrFovf fov{};
	};

	const char* to_string(XrResult result);
	const char* to_string(XrSessionState state);

	// Owns the OpenXR instance, session and the swapchain the game's image is copied into.
	// The render thread drives it; the only calls allowed from other threads are locate() and predicted_time(),
	// and only while the caller guarantees the object isn't being destroyed.
	class xr_runtime
	{
	public:
		// Creates the OpenXR instance; throws xr_error when no usable runtime is active
		xr_runtime();
		~xr_runtime();

		xr_runtime(const xr_runtime&) = delete;
		xr_runtime& operator=(const xr_runtime&) = delete;

		// Finds the headset and creates a session on the game's D3D11 device.
		// Returns false while the headset isn't available yet; throws xr_error on hard failures.
		bool try_create_session(ID3D11Device* device);
		bool has_session() const;

		// Pumps runtime events. Returns false once the runtime wants the session gone.
		bool poll_events();
		bool is_session_running() const;

		// Blocks until the headset wants the next frame. Call WITHOUT holding the game's D3D11 lock.
		bool wait_frame();

		// Begins the frame, copies `source` (may be null) and ends the frame. With `projection` the image is shown
		// head-tracked from the pose it was rendered with; without it, on the virtual screen.
		// Only valid after wait_frame() returned true. Call WITH the game's D3D11 lock held.
		void submit_frame(ID3D11DeviceContext* context, ID3D11Texture2D* source, const projection_info* projection,
			const screen_params& params);

		// Head and eye poses for `time` (thread-safe)
		bool locate(XrTime time, view_state& out) const;

		// Display time roughly `frames_ahead` frames after the frame the runtime last asked for; 0 if unknown
		XrTime predicted_time(float frames_ahead) const;

		// Counts runtime recentres (e.g. holding the Meta button); head tracking re-baselines when it changes
		std::uint32_t space_change_count() const
		{
			return this->space_changes_;
		}

		void request_recenter();
		std::string status() const;

	private:
		struct screen_swapchain
		{
			XrSwapchain handle = XR_NULL_HANDLE;
			std::vector<ID3D11Texture2D*> images;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool srgb = false;
		};

		XrInstance instance_ = XR_NULL_HANDLE;
		XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
		XrSession session_ = XR_NULL_HANDLE;
		XrSpace local_space_ = XR_NULL_HANDLE;
		XrSpace view_space_ = XR_NULL_HANDLE;

		XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
		std::atomic_bool session_running_ = false;
		bool frame_waited_ = false;
		XrFrameState frame_state_{XR_TYPE_FRAME_STATE};
		std::atomic<XrTime> last_display_time_ = 0;
		std::atomic<XrDuration> display_period_ = 0;
		std::atomic<std::uint32_t> space_changes_ = 0;
		std::uint64_t projected_frames_ = 0;
		XrEnvironmentBlendMode blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

		std::vector<std::int64_t> swapchain_formats_;
		screen_swapchain screen_;

		XrPosef anchor_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		bool recenter_requested_ = true;

		std::string runtime_name_ = "unknown";
		std::string system_name_;
		std::uint32_t eye_width_ = 0;
		std::uint32_t eye_height_ = 0;
		std::uint64_t frames_submitted_ = 0;
		bool waiting_for_headset_logged_ = false;
		std::unordered_set<std::string> reported_problems_;

		void log_system_properties();
		void query_swapchain_formats();
		void pick_blend_mode();
		bool handle_state_change(XrSessionState state);

		bool ensure_screen_swapchain(const D3D11_TEXTURE2D_DESC& source, bool srgb);
		void destroy_screen_swapchain();
		bool copy_to_screen(ID3D11DeviceContext* context, ID3D11Texture2D* source, bool srgb);
		void update_anchor(XrTime time);
		XrPosef screen_pose(const screen_params& params) const;

		// logs each distinct problem once, so a per-frame failure can't flood the log
		void report_once(const std::string& problem);
	};
}
