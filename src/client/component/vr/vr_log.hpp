#pragma once

#include <format>
#include <string>

// Logging for the VR component.
// Every line goes to <game folder>\iw7-vr.log (flushed immediately, so it survives crashes) and to the in-game console.
// The previous run's log is kept as iw7-vr.old.log.
namespace vr::log
{
	void init();
	void write(const char* level, const std::string& message);

	template <typename... Args>
	void info(std::format_string<Args...> fmt, Args&&... args)
	{
		write("info", std::format(fmt, std::forward<Args>(args)...));
	}

	template <typename... Args>
	void warn(std::format_string<Args...> fmt, Args&&... args)
	{
		write("warn", std::format(fmt, std::forward<Args>(args)...));
	}

	template <typename... Args>
	void error(std::format_string<Args...> fmt, Args&&... args)
	{
		write("error", std::format(fmt, std::forward<Args>(args)...));
	}
}
