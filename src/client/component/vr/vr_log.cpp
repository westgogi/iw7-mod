#include <std_include.hpp>

#include "vr_log.hpp"

#include "component/console/console.hpp"

namespace vr::log
{
	namespace
	{
		std::mutex log_mutex;
		std::ofstream log_file;

		std::filesystem::path game_folder()
		{
			char path[MAX_PATH]{};
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			return std::filesystem::path(path).parent_path();
		}

		std::string timestamp()
		{
			SYSTEMTIME time{};
			GetLocalTime(&time);
			return std::format("{:02}:{:02}:{:02}.{:03}", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
		}
	}

	void init()
	{
		std::lock_guard _(log_mutex);
		if (log_file.is_open())
		{
			return;
		}

		const auto folder = game_folder();
		const auto current = folder / "iw7-vr.log";
		const auto previous = folder / "iw7-vr.old.log";

		// keep the previous run around, it's usually the interesting one after a crash
		std::error_code ec;
		std::filesystem::remove(previous, ec);
		std::filesystem::rename(current, previous, ec);

		log_file.open(current, std::ios::out | std::ios::trunc);
	}

	void write(const char* level, const std::string& message)
	{
		{
			std::lock_guard _(log_mutex);
			if (log_file.is_open())
			{
				log_file << '[' << timestamp() << "] [" << level << "] " << message << '\n';
				log_file.flush();
			}
		}

		if (std::strcmp(level, "error") == 0)
		{
			console::error("[VR] %s\n", message.data());
		}
		else if (std::strcmp(level, "warn") == 0)
		{
			console::warn("[VR] %s\n", message.data());
		}
		else
		{
			console::info("[VR] %s\n", message.data());
		}
	}
}
