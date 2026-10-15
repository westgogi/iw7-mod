#include <std_include.hpp>

#include "xr_runtime.hpp"
#include "xr_math.hpp"
#include "vr_log.hpp"
#include "input.hpp"

#include <openxr/openxr_reflection.h>

#include <array>

namespace vr
{
	namespace
	{
		template <std::size_t Size>
		void copy_string(char (&destination)[Size], const char* source)
		{
			const auto length = std::min(std::strlen(source), Size - 1);
			std::memcpy(destination, source, length);
			destination[length] = '\0';
		}

		void check(const XrResult result, const char* what, const bool retry = false)
		{
			if (XR_FAILED(result))
			{
				throw xr_error(std::format("{} failed: {}", what, to_string(result)), retry);
			}
		}

		std::string version_string(const XrVersion version)
		{
			return std::format("{}.{}.{}", XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), XR_VERSION_PATCH(version));
		}

		const char* format_name(const std::int64_t format)
		{
			switch (static_cast<DXGI_FORMAT>(format))
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
			case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
			case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
			case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
			case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
			case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
			case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
			case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
			case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
			case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
			case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
			default: return "other";
			}
		}

		// Formats the runtime could give us that CopyResource accepts from the game's back buffer
		// (same "typeless family"). The sRGB variant comes first when the image is gamma encoded,
		// otherwise the headset would show it washed out.
		std::vector<DXGI_FORMAT> screen_format_candidates(const DXGI_FORMAT source, const bool srgb)
		{
			switch (source)
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return srgb
					? std::vector{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM}
					: std::vector{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return srgb
					? std::vector{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}
					: std::vector{DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return {DXGI_FORMAT_R10G10B10A2_UNORM};
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				return {DXGI_FORMAT_R16G16B16A16_FLOAT};
			default:
				return {};
			}
		}

		struct adapter_info
		{
			std::string name = "unknown";
			std::optional<LUID> luid;
		};

		adapter_info describe_adapter(ID3D11Device* device)
		{
			adapter_info info{};

			IDXGIDevice* dxgi_device = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))))
			{
				return info;
			}

			IDXGIAdapter* adapter = nullptr;
			if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)))
			{
				DXGI_ADAPTER_DESC desc{};
				if (SUCCEEDED(adapter->GetDesc(&desc)))
				{
					char name[128]{};
					WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);
					info.name = std::format("{} ({} MB VRAM)", name, desc.DedicatedVideoMemory / (1024 * 1024));
					info.luid = desc.AdapterLuid;
				}

				adapter->Release();
			}

			dxgi_device->Release();
			return info;
		}
	}

	const char* to_string(const XrResult result)
	{
		switch (result)
		{
#define IW7VR_ENUM_CASE(name, value) case name: return #name;
			XR_LIST_ENUM_XrResult(IW7VR_ENUM_CASE)
#undef IW7VR_ENUM_CASE
		default:
			return XR_SUCCEEDED(result) ? "XR_UNKNOWN_SUCCESS" : "XR_UNKNOWN_FAILURE";
		}
	}

	const char* to_string(const XrSessionState state)
	{
		switch (state)
		{
#define IW7VR_ENUM_CASE(name, value) case name: return #name;
			XR_LIST_ENUM_XrSessionState(IW7VR_ENUM_CASE)
#undef IW7VR_ENUM_CASE
		default:
			return "XR_SESSION_STATE_?";
		}
	}

	xr_runtime::xr_runtime()
	{
		std::uint32_t count = 0;
		auto result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
		if (XR_FAILED(result))
		{
			throw xr_error(std::format("No OpenXR runtime is available ({}). Start Quest Link / Air Link, Virtual Desktop or SteamVR "
				"and make sure it is set as the active OpenXR runtime.", to_string(result)), true);
		}

		std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
		check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "xrEnumerateInstanceExtensionProperties");

		auto has_d3d11 = false;
		std::string names;
		for (const auto& extension : extensions)
		{
			if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
			{
				has_d3d11 = true;
			}
			else if (std::strcmp(extension.extensionName, XR_META_PERFORMANCE_METRICS_EXTENSION_NAME) == 0)
			{
				this->has_performance_metrics_ = true;
			}
			else if (std::strcmp(extension.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0)
			{
				this->has_refresh_rate_ = true;
			}

			names += extension.extensionName;
			names += ' ';
		}

		log::info("Runtime offers {} extensions: {}", count, names);

		if (!has_d3d11)
		{
			throw xr_error("The active OpenXR runtime doesn't support Direct3D 11 (XR_KHR_D3D11_enable), which this game needs.", false);
		}

		std::vector<const char*> enabled_extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
		if (this->has_performance_metrics_)
		{
			enabled_extensions.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
		}

		if (this->has_refresh_rate_)
		{
			enabled_extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
		}

		XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
		copy_string(create_info.applicationInfo.applicationName, "IW7-VR");
		create_info.applicationInfo.applicationVersion = 1;
		copy_string(create_info.applicationInfo.engineName, "IW7");
		create_info.applicationInfo.engineVersion = 7;
		create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		create_info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size());
		create_info.enabledExtensionNames = enabled_extensions.data();

		result = xrCreateInstance(&create_info, &this->instance_);
		if (result == XR_ERROR_API_VERSION_UNSUPPORTED)
		{
			// some older runtimes compare the patch number too
			create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
			result = xrCreateInstance(&create_info, &this->instance_);
		}

		check(result, "xrCreateInstance", true);

		XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
		if (XR_SUCCEEDED(xrGetInstanceProperties(this->instance_, &properties)))
		{
			this->runtime_name_ = std::format("{} {}", properties.runtimeName, version_string(properties.runtimeVersion));
		}

		log::info("OpenXR runtime: {}", this->runtime_name_);

		try
		{
			this->create_actions();
		}
		catch (const xr_error& e)
		{
			// not fatal: the game still works with mouse, keyboard or a real gamepad
			log::error("VR controllers unavailable: {}", e.what());
			if (this->action_set_ != XR_NULL_HANDLE)
			{
				xrDestroyActionSet(this->action_set_);
				this->action_set_ = XR_NULL_HANDLE;
			}
		}
	}

	xr_runtime::~xr_runtime()
	{
		this->destroy_screen_swapchain();

		if (this->view_space_ != XR_NULL_HANDLE)
		{
			xrDestroySpace(this->view_space_);
		}

		if (this->local_space_ != XR_NULL_HANDLE)
		{
			xrDestroySpace(this->local_space_);
		}

		if (this->session_ != XR_NULL_HANDLE)
		{
			xrDestroySession(this->session_);
		}

		if (this->action_set_ != XR_NULL_HANDLE)
		{
			xrDestroyActionSet(this->action_set_);
		}

		if (this->instance_ != XR_NULL_HANDLE)
		{
			xrDestroyInstance(this->instance_);
		}
	}

	bool xr_runtime::try_create_session(ID3D11Device* device)
	{
		if (this->session_ != XR_NULL_HANDLE)
		{
			return true;
		}

		XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
		system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

		const auto result = xrGetSystem(this->instance_, &system_info, &this->system_id_);
		if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
		{
			if (!this->waiting_for_headset_logged_)
			{
				log::info("Waiting for the headset. Is it connected (Link / Air Link / Virtual Desktop / Steam Link) and awake?");
				this->waiting_for_headset_logged_ = true;
			}

			return false;
		}

		check(result, "xrGetSystem");
		this->log_system_properties();

		// the spec requires this call before xrCreateSession
		PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
		check(xrGetInstanceProcAddr(this->instance_, "xrGetD3D11GraphicsRequirementsKHR",
			reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)), "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");

		XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
		check(get_requirements(this->instance_, this->system_id_, &requirements), "xrGetD3D11GraphicsRequirementsKHR");

		const auto adapter = describe_adapter(device);
		log::info("Game GPU: {}", adapter.name);

		if (adapter.luid && std::memcmp(&*adapter.luid, &requirements.adapterLuid, sizeof(LUID)) != 0)
		{
			log::warn("The game is rendering on a different GPU than the one the headset runtime uses. "
				"If this PC has two GPUs, set iw7-vr.exe to 'High performance' in Windows graphics settings.");
		}

		if (device->GetFeatureLevel() < requirements.minFeatureLevel)
		{
			throw xr_error(std::format("The headset runtime needs Direct3D feature level {:#x} but the game's device is {:#x}.",
				static_cast<unsigned>(requirements.minFeatureLevel), static_cast<unsigned>(device->GetFeatureLevel())), false);
		}

		XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
		binding.device = device;

		XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
		session_info.next = &binding;
		session_info.systemId = this->system_id_;
		check(xrCreateSession(this->instance_, &session_info, &this->session_), "xrCreateSession");

		XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		space_info.poseInReferenceSpace = math::identity_pose();

		space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		check(xrCreateReferenceSpace(this->session_, &space_info, &this->local_space_), "xrCreateReferenceSpace(LOCAL)");

		space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		check(xrCreateReferenceSpace(this->session_, &space_info, &this->view_space_), "xrCreateReferenceSpace(VIEW)");

		this->query_swapchain_formats();
		this->pick_blend_mode();
		this->attach_actions();
		this->setup_performance_metrics();
		this->setup_refresh_rate();

		log::info("Session created, waiting for the runtime to start it");
		return true;
	}

	bool xr_runtime::has_session() const
	{
		return this->session_ != XR_NULL_HANDLE;
	}

	bool xr_runtime::is_session_running() const
	{
		return this->session_running_;
	}

	void xr_runtime::log_system_properties()
	{
		XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
		if (XR_SUCCEEDED(xrGetSystemProperties(this->instance_, this->system_id_, &properties)))
		{
			this->system_name_ = properties.systemName;
			log::info("Headset: {} (vendor {:#x}), max swapchain {}x{}, max layers {}, rotation tracking: {}, position tracking: {}",
				properties.systemName, properties.vendorId,
				properties.graphicsProperties.maxSwapchainImageWidth, properties.graphicsProperties.maxSwapchainImageHeight,
				properties.graphicsProperties.maxLayerCount,
				properties.trackingProperties.orientationTracking ? "yes" : "no",
				properties.trackingProperties.positionTracking ? "yes" : "no");
		}

		std::uint32_t view_count = 0;
		if (XR_FAILED(xrEnumerateViewConfigurationViews(this->instance_, this->system_id_,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr)) || view_count == 0)
		{
			return;
		}

		std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(this->instance_, this->system_id_,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data())))
		{
			this->eye_width_ = views[0].recommendedImageRectWidth;
			this->eye_height_ = views[0].recommendedImageRectHeight;
			log::info("Recommended resolution per eye: {}x{} ({} views, max {}x{})", this->eye_width_, this->eye_height_,
				view_count, views[0].maxImageRectWidth, views[0].maxImageRectHeight);
		}
	}

	void xr_runtime::query_swapchain_formats()
	{
		std::uint32_t count = 0;
		check(xrEnumerateSwapchainFormats(this->session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats");

		this->swapchain_formats_.resize(count);
		check(xrEnumerateSwapchainFormats(this->session_, count, &count, this->swapchain_formats_.data()), "xrEnumerateSwapchainFormats");

		std::string list;
		for (const auto format : this->swapchain_formats_)
		{
			list += std::format("{}({}) ", format_name(format), format);
		}

		log::info("Runtime swapchain formats: {}", list);
	}

	void xr_runtime::pick_blend_mode()
	{
		this->blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

		std::uint32_t count = 0;
		if (XR_FAILED(xrEnumerateEnvironmentBlendModes(this->instance_, this->system_id_,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) || count == 0)
		{
			return;
		}

		std::vector<XrEnvironmentBlendMode> modes(count);
		if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(this->instance_, this->system_id_,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()))
			&& std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end())
		{
			this->blend_mode_ = modes[0];
		}
	}

	bool xr_runtime::poll_events()
	{
		if (this->instance_ == XR_NULL_HANDLE)
		{
			return false;
		}

		for (;;)
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			const auto result = xrPollEvent(this->instance_, &event);
			if (result == XR_EVENT_UNAVAILABLE)
			{
				return true;
			}

			if (XR_FAILED(result))
			{
				log::error("xrPollEvent failed: {}", to_string(result));
				return false;
			}

			switch (event.type)
			{
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				log::warn("The OpenXR runtime is shutting down");
				return false;

			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
				if (changed.session == this->session_ && !this->handle_state_change(changed.state))
				{
					return false;
				}

				break;
			}

			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
				// the player recentred with the headset's own button; move the screen with them
				this->recenter_requested_ = true;
				++this->space_changes_;
				break;

			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
				this->log_interaction_profiles();
				break;

			case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
			{
				const auto& changed = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
				log::info("Headset refresh rate changed: {:.0f} Hz -> {:.0f} Hz", changed.fromDisplayRefreshRate, changed.toDisplayRefreshRate);
				break;
			}

			default:
				break;
			}
		}
	}

	bool xr_runtime::handle_state_change(const XrSessionState state)
	{
		log::info("Session state: {} -> {}", to_string(this->session_state_), to_string(state));
		this->session_state_ = state;

		switch (state)
		{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
			begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

			const auto result = xrBeginSession(this->session_, &begin_info);
			if (XR_FAILED(result))
			{
				log::error("xrBeginSession failed: {}", to_string(result));
				return false;
			}

			this->session_running_ = true;
			this->recenter_requested_ = true;
			break;
		}

		case XR_SESSION_STATE_STOPPING:
			this->session_running_ = false;
			this->frame_waited_ = false;
			xrEndSession(this->session_);
			break;

		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			this->session_running_ = false;
			this->frame_waited_ = false;
			return false;

		default:
			break;
		}

		return true;
	}

	bool xr_runtime::wait_frame()
	{
		if (!this->session_running_)
		{
			return false;
		}

		XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
		this->frame_state_ = {XR_TYPE_FRAME_STATE};

		const auto result = xrWaitFrame(this->session_, &wait_info, &this->frame_state_);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrWaitFrame failed: {}", to_string(result)));
			return false;
		}

		this->frame_waited_ = true;
		this->last_display_time_ = this->frame_state_.predictedDisplayTime;
		this->display_period_ = this->frame_state_.predictedDisplayPeriod;
		return true;
	}

	XrTime xr_runtime::predicted_time(const float frames_ahead) const
	{
		const auto last = this->last_display_time_.load();
		if (last == 0)
		{
			return 0;
		}

		return last + static_cast<XrTime>(static_cast<double>(this->display_period_.load()) * frames_ahead);
	}

	bool xr_runtime::locate(const XrTime time, view_state& out) const
	{
		if (!this->session_running_ || time == 0)
		{
			return false;
		}

		XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = time;
		locate_info.space = this->local_space_;

		XrViewState view_state_info{XR_TYPE_VIEW_STATE};
		std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
		std::uint32_t count = 0;
		if (XR_FAILED(xrLocateViews(this->session_, &locate_info, &view_state_info, 2, &count, views.data())) || count != 2)
		{
			return false;
		}

		constexpr XrViewStateFlags views_valid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
		if ((view_state_info.viewStateFlags & views_valid) != views_valid)
		{
			return false;
		}

		XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
		if (XR_FAILED(xrLocateSpace(this->view_space_, this->local_space_, time, &head)))
		{
			return false;
		}

		constexpr XrSpaceLocationFlags head_valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
		if ((head.locationFlags & head_valid) != head_valid)
		{
			return false;
		}

		out.time = time;
		out.head = head.pose;
		for (auto i = 0u; i < 2; ++i)
		{
			out.eye_pose[i] = views[i].pose;
			out.eye_fov[i] = views[i].fov;
		}

		return true;
	}

	void xr_runtime::submit_frame(ID3D11DeviceContext* context, ID3D11Texture2D* source, const projection_info* projection,
		const screen_params& params)
	{
		if (!this->frame_waited_)
		{
			return;
		}

		this->frame_waited_ = false;

		XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
		auto result = xrBeginFrame(this->session_, &begin_info);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrBeginFrame failed: {}", to_string(result)));
			return;
		}

		XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
		XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		std::array<XrCompositionLayerProjectionView, 2> projection_views{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
		std::array<XrCompositionLayerBaseHeader*, 1> layers{};
		std::uint32_t layer_count = 0;

		if (this->frame_state_.shouldRender && source && this->copy_to_screen(context, source, params.srgb))
		{
			XrSwapchainSubImage image{};
			image.swapchain = this->screen_.handle;
			image.imageRect.offset = {0, 0};
			image.imageRect.extent = {static_cast<std::int32_t>(this->screen_.width), static_cast<std::int32_t>(this->screen_.height)};
			image.imageArrayIndex = 0;

			if (projection)
			{
				// Head tracking: the game rendered this image from `projection->pose` with `projection->fov`.
				// Both eyes get the same picture for now (no stereo yet); the runtime re-projects it to where the head is now.
				for (auto& view : projection_views)
				{
					view.pose = projection->pose;
					view.fov = projection->fov;
					view.subImage = image;
				}

				projection_layer.layerFlags = 0;
				projection_layer.space = this->local_space_;
				projection_layer.viewCount = static_cast<std::uint32_t>(projection_views.size());
				projection_layer.views = projection_views.data();
				layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer);

				if (this->projected_frames_++ == 0)
				{
					log::info("First head-tracked frame shown (fov L{:.1f} R{:.1f} U{:.1f} D{:.1f} degrees)",
						projection->fov.angleLeft * math::to_degrees, projection->fov.angleRight * math::to_degrees,
						projection->fov.angleUp * math::to_degrees, projection->fov.angleDown * math::to_degrees);
				}
			}
			else
			{
				this->update_anchor(this->frame_state_.predictedDisplayTime);

				const auto aspect = static_cast<float>(this->screen_.height) / static_cast<float>(this->screen_.width);

				quad.layerFlags = 0; // ignore the game's alpha channel, the screen is opaque
				quad.space = params.follow_head ? this->view_space_ : this->local_space_;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage = image;
				quad.pose = this->screen_pose(params);
				quad.size = {params.width, params.width * aspect};

				layers[layer_count++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
			}
		}

		XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
		end_info.displayTime = this->frame_state_.predictedDisplayTime;
		end_info.environmentBlendMode = this->blend_mode_;
		end_info.layerCount = layer_count;
		end_info.layers = layers.data();

		result = xrEndFrame(this->session_, &end_info);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrEndFrame failed: {}", to_string(result)));
			return;
		}

		if (layer_count > 0 && this->frames_submitted_++ == 0)
		{
			log::info("First frame shown in the headset");
		}
	}

	bool xr_runtime::ensure_screen_swapchain(const D3D11_TEXTURE2D_DESC& source, const bool srgb)
	{
		if (this->screen_.handle != XR_NULL_HANDLE
			&& this->screen_.width == source.Width && this->screen_.height == source.Height
			&& this->screen_.source_format == source.Format && this->screen_.srgb == srgb)
		{
			return true;
		}

		this->destroy_screen_swapchain();

		auto format = DXGI_FORMAT_UNKNOWN;
		for (const auto candidate : screen_format_candidates(source.Format, srgb))
		{
			if (std::find(this->swapchain_formats_.begin(), this->swapchain_formats_.end(), static_cast<std::int64_t>(candidate)) != this->swapchain_formats_.end())
			{
				format = candidate;
				break;
			}
		}

		if (format == DXGI_FORMAT_UNKNOWN)
		{
			this->report_once(std::format("No headset swapchain format can take the game's image directly (game format {} ({})). "
				"This needs a shader conversion, which isn't written yet.", format_name(source.Format), static_cast<int>(source.Format)));
			return false;
		}

		XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		create_info.format = format;
		create_info.sampleCount = 1;
		create_info.width = source.Width;
		create_info.height = source.Height;
		create_info.faceCount = 1;
		create_info.arraySize = 1;
		create_info.mipCount = 1;

		auto result = xrCreateSwapchain(this->session_, &create_info, &this->screen_.handle);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrCreateSwapchain for the virtual screen failed: {}", to_string(result)));
			this->screen_.handle = XR_NULL_HANDLE;
			return false;
		}

		std::uint32_t count = 0;
		result = xrEnumerateSwapchainImages(this->screen_.handle, 0, &count, nullptr);

		std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
		if (XR_SUCCEEDED(result))
		{
			result = xrEnumerateSwapchainImages(this->screen_.handle, count, &count,
				reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
		}

		if (XR_FAILED(result) || count == 0)
		{
			this->report_once(std::format("xrEnumerateSwapchainImages failed: {}", to_string(result)));
			this->destroy_screen_swapchain();
			return false;
		}

		for (const auto& image : images)
		{
			this->screen_.images.push_back(image.texture);
		}

		this->screen_.width = source.Width;
		this->screen_.height = source.Height;
		this->screen_.source_format = source.Format;
		this->screen_.format = format;
		this->screen_.srgb = srgb;

		log::info("Virtual screen: {}x{}, game image {} ({} samples) -> headset {} ({} images)",
			source.Width, source.Height, format_name(source.Format), source.SampleDesc.Count, format_name(format), count);
		return true;
	}

	void xr_runtime::destroy_screen_swapchain()
	{
		if (this->screen_.handle != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(this->screen_.handle);
		}

		this->screen_ = {};
	}

	bool xr_runtime::copy_to_screen(ID3D11DeviceContext* context, ID3D11Texture2D* source, const bool srgb)
	{
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);

		if (!this->ensure_screen_swapchain(desc, srgb))
		{
			return false;
		}

		std::uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		auto result = xrAcquireSwapchainImage(this->screen_.handle, &acquire_info, &index);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrAcquireSwapchainImage failed: {}", to_string(result)));
			return false;
		}

		XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait_info.timeout = XR_INFINITE_DURATION;
		result = xrWaitSwapchainImage(this->screen_.handle, &wait_info);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrWaitSwapchainImage failed: {}", to_string(result)));
			return false;
		}

		auto* target = this->screen_.images[index];
		if (desc.SampleDesc.Count > 1)
		{
			context->ResolveSubresource(target, 0, source, 0, this->screen_.format);
		}
		else
		{
			context->CopyResource(target, source);
		}

		XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		result = xrReleaseSwapchainImage(this->screen_.handle, &release_info);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrReleaseSwapchainImage failed: {}", to_string(result)));
			return false;
		}

		return true;
	}

	void xr_runtime::request_recenter()
	{
		this->recenter_requested_ = true;
	}

	void xr_runtime::update_anchor(const XrTime time)
	{
		if (!this->recenter_requested_)
		{
			return;
		}

		XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
		if (XR_FAILED(xrLocateSpace(this->view_space_, this->local_space_, time, &location)))
		{
			return;
		}

		constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
		if ((location.locationFlags & required) != required)
		{
			return; // tracking not ready yet, try again next frame
		}

		// keep only the heading, so the screen stays upright whatever angle the head was at
		this->anchor_.position = location.pose.position;
		this->anchor_.orientation = math::yaw_rotation(math::yaw_of(location.pose.orientation));
		this->recenter_requested_ = false;

		log::info("Screen recentred in front of the headset");
	}

	XrPosef xr_runtime::screen_pose(const screen_params& params) const
	{
		const XrVector3f offset{0.0f, params.height_offset, -params.distance};

		if (params.follow_head)
		{
			XrPosef pose = math::identity_pose();
			pose.position = offset;
			return pose;
		}

		XrPosef pose{};
		pose.orientation = this->anchor_.orientation;
		pose.position = math::add(this->anchor_.position, math::rotate(this->anchor_.orientation, offset));
		return pose;
	}

	std::string xr_runtime::path_string(const XrPath path) const
	{
		char buffer[XR_MAX_PATH_LENGTH]{};
		std::uint32_t length = 0;
		if (XR_FAILED(xrPathToString(this->instance_, path, static_cast<std::uint32_t>(sizeof(buffer)), &length, buffer)))
		{
			return "?";
		}

		return buffer;
	}

	// ---- controllers --------------------------------------------------------------------------------------------------

	void xr_runtime::create_actions()
	{
		check(xrStringToPath(this->instance_, "/user/hand/left", &this->hand_paths_[0]), "xrStringToPath");
		check(xrStringToPath(this->instance_, "/user/hand/right", &this->hand_paths_[1]), "xrStringToPath");

		XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
		copy_string(set_info.actionSetName, "gameplay");
		copy_string(set_info.localizedActionSetName, "Gameplay");
		set_info.priority = 0;
		check(xrCreateActionSet(this->instance_, &set_info, &this->action_set_), "xrCreateActionSet");

		const auto create = [this](XrAction& action, const char* name, const char* localized, const XrActionType type,
			const bool per_hand)
		{
			XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
			copy_string(info.actionName, name);
			copy_string(info.localizedActionName, localized);
			info.actionType = type;
			if (per_hand)
			{
				info.countSubactionPaths = 2;
				info.subactionPaths = this->hand_paths_;
			}

			check(xrCreateAction(this->action_set_, &info, &action), "xrCreateAction");
		};

		create(this->thumbstick_action_, "thumbstick", "Thumbsticks", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
		create(this->trigger_action_, "trigger", "Triggers", XR_ACTION_TYPE_FLOAT_INPUT, true);
		create(this->grip_action_, "grip", "Grips", XR_ACTION_TYPE_FLOAT_INPUT, true);
		create(this->thumbstick_click_action_, "thumbstick_click", "Thumbstick clicks", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
		create(this->button_a_action_, "button_a", "A button", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
		create(this->button_b_action_, "button_b", "B button", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
		create(this->button_x_action_, "button_x", "X button", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
		create(this->button_y_action_, "button_y", "Y button", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
		create(this->menu_action_, "menu", "Menu button", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
		create(this->haptic_action_, "rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);

		struct binding
		{
			XrAction action;
			const char* path;
		};

		const auto suggest = [this](const char* profile, const std::vector<binding>& bindings)
		{
			std::vector<XrActionSuggestedBinding> suggested;
			for (const auto& entry : bindings)
			{
				XrPath path = XR_NULL_PATH;
				check(xrStringToPath(this->instance_, entry.path, &path), "xrStringToPath");
				suggested.push_back({entry.action, path});
			}

			XrPath profile_path = XR_NULL_PATH;
			check(xrStringToPath(this->instance_, profile, &profile_path), "xrStringToPath");

			XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
			info.interactionProfile = profile_path;
			info.countSuggestedBindings = static_cast<std::uint32_t>(suggested.size());
			info.suggestedBindings = suggested.data();

			const auto result = xrSuggestInteractionProfileBindings(this->instance_, &info);
			if (XR_FAILED(result))
			{
				log::warn("The runtime rejected the controller bindings for {}: {}", profile, to_string(result));
				return false;
			}

			return true;
		};

		// Quest / Rift Touch controllers (Quest 3's Touch Plus controllers use this profile too)
		const auto touch = suggest("/interaction_profiles/oculus/touch_controller", {
			{this->thumbstick_action_, "/user/hand/left/input/thumbstick"},
			{this->thumbstick_action_, "/user/hand/right/input/thumbstick"},
			{this->trigger_action_, "/user/hand/left/input/trigger/value"},
			{this->trigger_action_, "/user/hand/right/input/trigger/value"},
			{this->grip_action_, "/user/hand/left/input/squeeze/value"},
			{this->grip_action_, "/user/hand/right/input/squeeze/value"},
			{this->thumbstick_click_action_, "/user/hand/left/input/thumbstick/click"},
			{this->thumbstick_click_action_, "/user/hand/right/input/thumbstick/click"},
			{this->button_a_action_, "/user/hand/right/input/a/click"},
			{this->button_b_action_, "/user/hand/right/input/b/click"},
			{this->button_x_action_, "/user/hand/left/input/x/click"},
			{this->button_y_action_, "/user/hand/left/input/y/click"},
			{this->menu_action_, "/user/hand/left/input/menu/click"},
			{this->haptic_action_, "/user/hand/left/output/haptic"},
			{this->haptic_action_, "/user/hand/right/output/haptic"},
		});

		// Valve Index controllers: the left hand's A/B stand in for X/Y; there is no free menu button
		suggest("/interaction_profiles/valve/index_controller", {
			{this->thumbstick_action_, "/user/hand/left/input/thumbstick"},
			{this->thumbstick_action_, "/user/hand/right/input/thumbstick"},
			{this->trigger_action_, "/user/hand/left/input/trigger/value"},
			{this->trigger_action_, "/user/hand/right/input/trigger/value"},
			{this->grip_action_, "/user/hand/left/input/squeeze/value"},
			{this->grip_action_, "/user/hand/right/input/squeeze/value"},
			{this->thumbstick_click_action_, "/user/hand/left/input/thumbstick/click"},
			{this->thumbstick_click_action_, "/user/hand/right/input/thumbstick/click"},
			{this->button_a_action_, "/user/hand/right/input/a/click"},
			{this->button_b_action_, "/user/hand/right/input/b/click"},
			{this->button_x_action_, "/user/hand/left/input/a/click"},
			{this->button_y_action_, "/user/hand/left/input/b/click"},
			{this->haptic_action_, "/user/hand/left/output/haptic"},
			{this->haptic_action_, "/user/hand/right/output/haptic"},
		});

		if (!touch)
		{
			throw xr_error("the runtime rejected the Touch controller bindings", false);
		}
	}

	void xr_runtime::attach_actions()
	{
		if (this->action_set_ == XR_NULL_HANDLE || this->actions_attached_)
		{
			return;
		}

		XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		info.countActionSets = 1;
		info.actionSets = &this->action_set_;

		const auto result = xrAttachSessionActionSets(this->session_, &info);
		if (XR_FAILED(result))
		{
			log::error("Couldn't attach the VR controller actions: {}", to_string(result));
			return;
		}

		this->actions_attached_ = true;
	}

	void xr_runtime::log_interaction_profiles()
	{
		if (!this->actions_attached_)
		{
			return;
		}

		for (auto hand = 0; hand < 2; ++hand)
		{
			XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
			if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(this->session_, this->hand_paths_[hand], &state)))
			{
				log::info("{} controller: {}", hand == 0 ? "Left" : "Right",
					state.interactionProfile == XR_NULL_PATH ? "none" : this->path_string(state.interactionProfile));
			}
		}
	}

	void xr_runtime::sync_input(input::controller_state& out)
	{
		out = {};

		if (!this->actions_attached_ || !this->session_running_)
		{
			return;
		}

		out.connected = this->controllers_seen_;
		if (this->session_state_ != XR_SESSION_STATE_FOCUSED)
		{
			return;
		}

		XrActiveActionSet active{this->action_set_, XR_NULL_PATH};
		XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
		sync_info.countActiveActionSets = 1;
		sync_info.activeActionSets = &active;

		const auto result = xrSyncActions(this->session_, &sync_info);
		if (XR_FAILED(result))
		{
			this->report_once(std::format("xrSyncActions failed: {}", to_string(result)));
			return;
		}

		if (result != XR_SUCCESS)
		{
			return; // XR_SESSION_NOT_FOCUSED: another app (the Meta menu) has the controllers
		}

		auto any_active = false;

		const auto read_float = [&](const XrAction action, const XrPath hand)
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			info.subactionPath = hand;

			XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetActionStateFloat(this->session_, &info, &state)) || !state.isActive)
			{
				return 0.0f;
			}

			any_active = true;
			return state.currentState;
		};

		const auto read_bool = [&](const XrAction action, const XrPath hand)
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			info.subactionPath = hand;

			XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_FAILED(xrGetActionStateBoolean(this->session_, &info, &state)) || !state.isActive)
			{
				return false;
			}

			any_active = true;
			return state.currentState == XR_TRUE;
		};

		const auto read_stick = [&](const XrAction action, const XrPath hand, float (&value)[2])
		{
			XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
			info.action = action;
			info.subactionPath = hand;

			XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
			if (XR_FAILED(xrGetActionStateVector2f(this->session_, &info, &state)) || !state.isActive)
			{
				return;
			}

			any_active = true;
			value[0] = state.currentState.x;
			value[1] = state.currentState.y;
		};

		const auto left = this->hand_paths_[0];
		const auto right = this->hand_paths_[1];

		read_stick(this->thumbstick_action_, left, out.left_stick);
		read_stick(this->thumbstick_action_, right, out.right_stick);
		out.left_trigger = read_float(this->trigger_action_, left);
		out.right_trigger = read_float(this->trigger_action_, right);
		out.left_grip = read_float(this->grip_action_, left);
		out.right_grip = read_float(this->grip_action_, right);
		out.left_stick_click = read_bool(this->thumbstick_click_action_, left);
		out.right_stick_click = read_bool(this->thumbstick_click_action_, right);
		out.a = read_bool(this->button_a_action_, XR_NULL_PATH);
		out.b = read_bool(this->button_b_action_, XR_NULL_PATH);
		out.x = read_bool(this->button_x_action_, XR_NULL_PATH);
		out.y = read_bool(this->button_y_action_, XR_NULL_PATH);
		out.menu = read_bool(this->menu_action_, XR_NULL_PATH);
		out.active = any_active;

		if (any_active)
		{
			this->controllers_seen_ = true;
			out.connected = true;
		}

		++this->input_syncs_;
		if (any_active && !this->controllers_logged_)
		{
			this->controllers_logged_ = true;
			log::info("VR controllers active");
			this->log_interaction_profiles();
		}
	}

	void xr_runtime::apply_haptics(const float left, const float right)
	{
		if (!this->actions_attached_ || this->session_state_ != XR_SESSION_STATE_FOCUSED)
		{
			return;
		}

		const float amplitude[2]{left, right};
		for (auto hand = 0; hand < 2; ++hand)
		{
			XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
			info.action = this->haptic_action_;
			info.subactionPath = this->hand_paths_[hand];

			if (amplitude[hand] > 0.01f)
			{
				XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
				vibration.amplitude = std::min(amplitude[hand], 1.0f);
				vibration.duration = 50'000'000; // 50 ms, renewed every frame for as long as the game keeps rumbling
				vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
				xrApplyHapticFeedback(this->session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
			}
			else if (this->haptic_amplitude_[hand] > 0.01f)
			{
				xrStopHapticFeedback(this->session_, &info);
			}

			this->haptic_amplitude_[hand] = amplitude[hand];
		}
	}

	// ---- runtime performance counters and refresh rate (Meta extensions) ------------------------------------------------

	void xr_runtime::setup_performance_metrics()
	{
		if (!this->has_performance_metrics_)
		{
			return;
		}

		PFN_xrEnumeratePerformanceMetricsCounterPathsMETA enumerate = nullptr;
		PFN_xrSetPerformanceMetricsStateMETA set_state = nullptr;
		if (XR_FAILED(xrGetInstanceProcAddr(this->instance_, "xrEnumeratePerformanceMetricsCounterPathsMETA",
				reinterpret_cast<PFN_xrVoidFunction*>(&enumerate)))
			|| XR_FAILED(xrGetInstanceProcAddr(this->instance_, "xrSetPerformanceMetricsStateMETA",
				reinterpret_cast<PFN_xrVoidFunction*>(&set_state)))
			|| XR_FAILED(xrGetInstanceProcAddr(this->instance_, "xrQueryPerformanceMetricsCounterMETA",
				reinterpret_cast<PFN_xrVoidFunction*>(&this->query_performance_counter_))))
		{
			this->query_performance_counter_ = nullptr;
			log::warn("The runtime lists XR_META_performance_metrics but its functions are missing");
			return;
		}

		XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
		state.enabled = XR_TRUE;
		auto result = set_state(this->session_, &state);
		if (XR_FAILED(result))
		{
			this->query_performance_counter_ = nullptr;
			log::warn("Couldn't enable the runtime's performance counters: {}", to_string(result));
			return;
		}

		std::uint32_t count = 0;
		result = enumerate(this->instance_, 0, &count, nullptr);
		if (XR_FAILED(result) || count == 0)
		{
			return;
		}

		std::vector<XrPath> paths(count);
		if (XR_FAILED(enumerate(this->instance_, count, &count, paths.data())))
		{
			return;
		}

		std::string names;
		for (const auto path : paths)
		{
			auto name = this->path_string(path);
			names += name;
			names += ' ';
			this->performance_counters_.emplace_back(path, std::move(name));
		}

		log::info("Runtime performance counters: {}", names);
	}

	std::string xr_runtime::performance_report()
	{
		if (!this->query_performance_counter_ || this->performance_counters_.empty() || !this->session_running_)
		{
			return {};
		}

		std::string report;
		for (const auto& [path, name] : this->performance_counters_)
		{
			XrPerformanceMetricsCounterMETA counter{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
			if (XR_FAILED(this->query_performance_counter_(this->session_, path, &counter))
				|| !(counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META))
			{
				continue;
			}

			std::string_view short_name = name;
			constexpr std::string_view prefix = "/perfmetrics_meta/";
			if (short_name.starts_with(prefix))
			{
				short_name.remove_prefix(prefix.size());
			}

			const char* unit = "";
			switch (counter.counterUnit)
			{
			case XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META: unit = "%"; break;
			case XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META: unit = " ms"; break;
			case XR_PERFORMANCE_METRICS_COUNTER_UNIT_BYTES_META: unit = " B"; break;
			case XR_PERFORMANCE_METRICS_COUNTER_UNIT_HERTZ_META: unit = " Hz"; break;
			default: break;
			}

			if (counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)
			{
				report += std::format("{} {:.2f}{}, ", short_name, counter.floatValue, unit);
			}
			else
			{
				report += std::format("{} {}{}, ", short_name, counter.uintValue, unit);
			}
		}

		if (report.size() >= 2)
		{
			report.resize(report.size() - 2);
		}

		return report;
	}

	void xr_runtime::setup_refresh_rate()
	{
		if (!this->has_refresh_rate_)
		{
			return;
		}

		PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
		if (XR_FAILED(xrGetInstanceProcAddr(this->instance_, "xrEnumerateDisplayRefreshRatesFB",
				reinterpret_cast<PFN_xrVoidFunction*>(&enumerate)))
			|| XR_FAILED(xrGetInstanceProcAddr(this->instance_, "xrGetDisplayRefreshRateFB",
				reinterpret_cast<PFN_xrVoidFunction*>(&this->get_refresh_rate_)))
			|| XR_FAILED(xrGetInstanceProcAddr(this->instance_, "xrRequestDisplayRefreshRateFB",
				reinterpret_cast<PFN_xrVoidFunction*>(&this->request_refresh_rate_))))
		{
			this->get_refresh_rate_ = nullptr;
			this->request_refresh_rate_ = nullptr;
			log::warn("The runtime lists XR_FB_display_refresh_rate but its functions are missing");
			return;
		}

		std::uint32_t count = 0;
		if (XR_SUCCEEDED(enumerate(this->session_, 0, &count, nullptr)) && count > 0)
		{
			this->refresh_rates_.resize(count);
			if (XR_FAILED(enumerate(this->session_, count, &count, this->refresh_rates_.data())))
			{
				this->refresh_rates_.clear();
			}
		}

		auto current = 0.0f;
		this->get_refresh_rate_(this->session_, &current);

		std::string rates;
		for (const auto rate : this->refresh_rates_)
		{
			rates += std::format("{:.0f} ", rate);
		}

		log::info("Headset refresh rate: {:.0f} Hz (available: {}); set vr_refresh_rate to change it", current, rates);
	}

	void xr_runtime::request_refresh_rate(const float hz)
	{
		if (!this->request_refresh_rate_ || !this->session_running_ || hz <= 0.0f
			|| std::abs(hz - this->requested_refresh_rate_) < 0.5f)
		{
			return;
		}

		this->requested_refresh_rate_ = hz;

		const auto match = std::find_if(this->refresh_rates_.begin(), this->refresh_rates_.end(), [hz](const float rate)
		{
			return std::abs(rate - hz) < 0.5f;
		});

		if (match == this->refresh_rates_.end())
		{
			std::string rates;
			for (const auto rate : this->refresh_rates_)
			{
				rates += std::format("{:.0f} ", rate);
			}

			log::warn("The headset doesn't offer {:.0f} Hz (available: {})", hz, rates);
			return;
		}

		const auto result = this->request_refresh_rate_(this->session_, *match);
		if (XR_FAILED(result))
		{
			log::warn("Couldn't switch the headset to {:.0f} Hz: {}", hz, to_string(result));
		}
		else
		{
			log::info("Asked the headset for {:.0f} Hz", hz);
		}
	}

	void xr_runtime::report_once(const std::string& problem)
	{
		if (this->reported_problems_.insert(problem).second)
		{
			log::error("{}", problem);
		}
	}

	std::string xr_runtime::status() const
	{
		return std::format("runtime: {} | headset: {} | session: {} | state: {} | frames shown: {} ({} head-tracked) | image: {}x{} {} | controller reads: {}",
			this->runtime_name_,
			this->system_name_.empty() ? "not found yet" : this->system_name_,
			this->session_ != XR_NULL_HANDLE ? (this->session_running_ ? "running" : "created") : "none",
			to_string(this->session_state_),
			this->frames_submitted_, this->projected_frames_,
			this->screen_.width, this->screen_.height, format_name(this->screen_.format),
			this->actions_attached_ ? std::to_string(this->input_syncs_) : std::string("unavailable"));
	}
}
