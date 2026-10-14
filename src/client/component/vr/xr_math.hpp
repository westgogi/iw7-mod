#pragma once

#include <algorithm>
#include <cmath>

#include <openxr/openxr.h>

// Small vector/quaternion helpers in OpenXR's coordinate convention:
// metres, right-handed, +X right, +Y up, -Z forward.
namespace vr::math
{
	inline XrVector3f add(const XrVector3f& a, const XrVector3f& b)
	{
		return {a.x + b.x, a.y + b.y, a.z + b.z};
	}

	inline XrVector3f scale(const XrVector3f& v, const float s)
	{
		return {v.x * s, v.y * s, v.z * s};
	}

	// Hamilton product: the result applies `b` first, then `a`
	inline XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b)
	{
		return {
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
		};
	}

	inline XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v)
	{
		// v' = v + w * t + (q.xyz x t), where t = 2 * (q.xyz x v)
		const XrVector3f t{
			2.0f * (q.y * v.z - q.z * v.y),
			2.0f * (q.z * v.x - q.x * v.z),
			2.0f * (q.x * v.y - q.y * v.x),
		};

		return {
			v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
			v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
			v.z + q.w * t.z + (q.x * t.y - q.y * t.x),
		};
	}

	// Rotation of `radians` around +Y (positive turns left)
	inline XrQuaternionf yaw_rotation(const float radians)
	{
		return {0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)};
	}

	// Heading of the forward (-Z) axis around +Y, in radians; 0 means looking down -Z
	inline float yaw_of(const XrQuaternionf& q)
	{
		const auto forward = rotate(q, {0.0f, 0.0f, -1.0f});
		return std::atan2(-forward.x, -forward.z);
	}

	inline XrPosef identity_pose()
	{
		XrPosef pose{};
		pose.orientation.w = 1.0f;
		return pose;
	}

	inline XrVector3f subtract(const XrVector3f& a, const XrVector3f& b)
	{
		return {a.x - b.x, a.y - b.y, a.z - b.z};
	}

	constexpr float pi = 3.14159265358979f;
	constexpr float to_degrees = 180.0f / pi;
	constexpr float to_radians = pi / 180.0f;

	// wraps an angle in degrees into [-180, 180)
	inline float wrap_degrees(const float degrees)
	{
		return degrees - 360.0f * std::floor((degrees + 180.0f) / 360.0f);
	}

	// ---- game (IW) convention -------------------------------------------------------------------
	// Quake-style world: +X forward, +Y left, +Z up, units are inches (about 39.37 per metre).
	// Angles in degrees: pitch (positive looks down), yaw (positive turns left), roll.
	// Verified against the game's own AnglesToAxis (0x140CE6240): axis[0] forward, axis[1] left, axis[2] up.

	struct vec3
	{
		float x, y, z;
	};

	// direction/offset from OpenXR space into game space (no scaling)
	inline vec3 to_game(const XrVector3f& v)
	{
		return {-v.z, -v.x, v.y};
	}

	inline vec3 rotate_yaw(const vec3& v, const float degrees)
	{
		const auto c = std::cos(degrees * to_radians);
		const auto s = std::sin(degrees * to_radians);
		return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
	}

	// game-space axes (forward, left, up) of an OpenXR orientation
	inline void to_game_axis(const XrQuaternionf& q, vec3 (&axis)[3])
	{
		axis[0] = to_game(rotate(q, {0.0f, 0.0f, -1.0f}));
		axis[1] = to_game(rotate(q, {-1.0f, 0.0f, 0.0f}));
		axis[2] = to_game(rotate(q, {0.0f, 1.0f, 0.0f}));
	}

	// game-convention yaw and pitch (degrees) of an OpenXR orientation
	inline void game_yaw_pitch(const XrQuaternionf& q, float& yaw, float& pitch)
	{
		const auto forward = to_game(rotate(q, {0.0f, 0.0f, -1.0f}));
		yaw = std::atan2(forward.y, forward.x) * to_degrees;
		pitch = -std::asin(std::clamp(forward.z, -1.0f, 1.0f)) * to_degrees;
	}
}
