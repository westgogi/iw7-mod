-- OpenXR loader, built as a static library so iw7-vr ships as a single exe.
-- Mirrors the source list and defines of deps/OpenXR-SDK/src/loader/CMakeLists.txt (Windows, static, no API layers bundled).

openxr = {
	source = path.join(dependencies.basePath, "OpenXR-SDK"),
}

function openxr.import()
	links { "openxr" }
	openxr.includes()
end

function openxr.includes()
	includedirs {
		path.join(openxr.source, "include"),
	}
end

function openxr.project()
	project "openxr"
		language "C++"

		openxr.includes()

		includedirs {
			path.join(openxr.source, "src"),
			path.join(openxr.source, "src/common"),
			path.join(openxr.source, "src/loader"),
			path.join(openxr.source, "src/external/jsoncpp/include"),
		}

		files {
			path.join(openxr.source, "src/loader/api_layer_interface.cpp"),
			path.join(openxr.source, "src/loader/loader_core.cpp"),
			path.join(openxr.source, "src/loader/loader_init_data.cpp"),
			path.join(openxr.source, "src/loader/loader_instance.cpp"),
			path.join(openxr.source, "src/loader/loader_logger.cpp"),
			path.join(openxr.source, "src/loader/loader_logger_recorders.cpp"),
			path.join(openxr.source, "src/loader/loader_properties.cpp"),
			path.join(openxr.source, "src/loader/manifest_file.cpp"),
			path.join(openxr.source, "src/loader/runtime_interface.cpp"),
			path.join(openxr.source, "src/loader/xr_generated_loader.cpp"),
			path.join(openxr.source, "src/common/filesystem_utils.cpp"),
			path.join(openxr.source, "src/common/object_info.cpp"),
			path.join(openxr.source, "src/xr_generated_dispatch_table_core.c"),
			path.join(openxr.source, "src/external/jsoncpp/src/lib_json/json_reader.cpp"),
			path.join(openxr.source, "src/external/jsoncpp/src/lib_json/json_value.cpp"),
			path.join(openxr.source, "src/external/jsoncpp/src/lib_json/json_writer.cpp"),
		}

		defines {
			"XR_OS_WINDOWS",
			"XR_USE_PLATFORM_WIN32",
			"_CRT_SECURE_NO_WARNINGS",
		}

		warnings "Off"
		kind "StaticLib"
end

table.insert(dependencies, openxr)
