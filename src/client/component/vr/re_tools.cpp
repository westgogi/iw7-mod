#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "vr_log.hpp"

#include "component/command.hpp"

#include "game/game.hpp"

// Reverse-engineering helpers for developing the VR mod.
//
// iw7_ship.exe is encrypted on disk and only decrypts itself in memory, so static analysis needs a memory dump.
// vr_dump_game writes the loaded game image to <game folder>\iw7_ship.dump.exe, laid out 1:1 with memory
// (file offset == RVA) and with the section table patched to match, so disassemblers load it at the right
// addresses. Run it from the main menu, once the game has fully started.

namespace vr::re_tools
{
	namespace
	{
		std::filesystem::path game_folder()
		{
			char path[MAX_PATH]{};
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			return std::filesystem::path(path).parent_path();
		}

		bool is_readable(const MEMORY_BASIC_INFORMATION& info)
		{
			constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
				| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

			return info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 && (info.Protect & readable) != 0;
		}

		std::size_t image_size(const std::uint8_t* base)
		{
			constexpr std::size_t fallback = 0xC301400; // SizeOfImage of the Steam iw7_ship.exe

			IMAGE_DOS_HEADER dos{};
			IMAGE_NT_HEADERS64 nt{};
			SIZE_T read = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), base, &dos, sizeof(dos), &read) || dos.e_magic != IMAGE_DOS_SIGNATURE
				|| !ReadProcessMemory(GetCurrentProcess(), base + dos.e_lfanew, &nt, sizeof(nt), &read) || nt.Signature != IMAGE_NT_SIGNATURE)
			{
				return fallback;
			}

			return nt.OptionalHeader.SizeOfImage;
		}

		// rewrite the section table so each section's file data sits at its virtual address
		void fix_section_table(std::vector<std::uint8_t>& headers)
		{
			if (headers.size() < sizeof(IMAGE_DOS_HEADER))
			{
				return;
			}

			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers.data());
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
				|| static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > headers.size())
			{
				return;
			}

			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(headers.data() + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
			{
				return;
			}

			auto* sections = IMAGE_FIRST_SECTION(nt);
			const auto end = reinterpret_cast<std::uint8_t*>(sections + nt->FileHeader.NumberOfSections);
			if (end > headers.data() + headers.size())
			{
				return;
			}

			nt->OptionalHeader.FileAlignment = nt->OptionalHeader.SectionAlignment;
			for (auto i = 0; i < nt->FileHeader.NumberOfSections; ++i)
			{
				sections[i].PointerToRawData = sections[i].VirtualAddress;
				sections[i].SizeOfRawData = sections[i].Misc.VirtualSize;
			}
		}

		void dump_game_image()
		{
			const auto* base = reinterpret_cast<const std::uint8_t*>(game::base_address);
			const auto size = image_size(base);
			const auto path = game_folder() / "iw7_ship.dump.exe";

			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				log::error("vr_dump_game: can't create {}", path.string());
				return;
			}

			std::vector<std::uint8_t> buffer(0x100000);
			std::vector<std::uint8_t> headers;
			std::size_t readable_bytes = 0;

			for (std::size_t offset = 0; offset < size;)
			{
				MEMORY_BASIC_INFORMATION info{};
				if (!VirtualQuery(base + offset, &info, sizeof(info)))
				{
					break;
				}

				const auto region_end = std::min(
					static_cast<std::size_t>(static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize - base), size);
				const auto readable = is_readable(info);

				while (offset < region_end)
				{
					const auto chunk = std::min(buffer.size(), region_end - offset);
					SIZE_T read = 0;
					if (!readable || !ReadProcessMemory(GetCurrentProcess(), base + offset, buffer.data(), chunk, &read) || read != chunk)
					{
						std::fill_n(buffer.begin(), chunk, std::uint8_t{0});
					}
					else
					{
						readable_bytes += chunk;
					}

					if (offset == 0)
					{
						headers.assign(buffer.begin(), buffer.begin() + std::min<std::size_t>(chunk, 0x1000));
					}

					out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
					offset += chunk;
				}
			}

			fix_section_table(headers);
			out.seekp(0);
			out.write(reinterpret_cast<const char*>(headers.data()), static_cast<std::streamsize>(headers.size()));
			out.close();

			log::info("vr_dump_game: wrote {} ({} MB, {} MB readable) from base {:#x}", path.string(), size / (1024 * 1024),
				readable_bytes / (1024 * 1024), game::base_address);
		}
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

			command::add("vr_dump_game", []
			{
				dump_game_image();
			});
		}
	};
}

REGISTER_COMPONENT(vr::re_tools::component)
