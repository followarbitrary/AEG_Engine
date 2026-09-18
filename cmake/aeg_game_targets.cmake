include_guard(GLOBAL)

function(aeg_add_game_module target)
    cmake_parse_arguments(ARG "" "METADATA;BUILD_ID;TARGET_PLATFORM;OUTPUT_DIRECTORY" "SOURCES" ${ARGN})
    if (TARGET ${target})
        message(FATAL_ERROR "aeg_add_game_module(${target}) would redefine an existing target")
    endif()
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "aeg_add_game_module(${target}) requires SOURCES")
    endif()
    add_library(${target} SHARED ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES AEG_GAME_MODULE_SOURCES "${ARG_SOURCES}")
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_link_libraries(${target} PRIVATE AEG::GameSDK)
    target_include_directories(${target} PRIVATE "${AEG_ENGINE_SOURCE_DIR}/Engine/Module/public"
        "${AEG_ENGINE_SOURCE_DIR}/Engine/Pipeline/public")
    target_compile_features(${target} PRIVATE cxx_std_20)

    # A module metadata sidecar is an explicit authoring input.  When supplied,
    # publish the DLL, canonical metadata and verified manifest together after
    # the DLL has linked.  The metadata tool is intentionally a build-time
    # dependency; it never becomes a shipping executable dependency.
    if (ARG_METADATA)
        if (NOT TARGET aeg_module_metadata_tool)
            message(FATAL_ERROR
                "aeg_add_game_module(${target}) METADATA requires AEG_BUILD_TOOLS=ON "
                "or AEG_BUILD_EDITOR=ON so aeg_module_metadata_tool is available")
        endif()
        if (NOT ARG_BUILD_ID OR NOT ARG_TARGET_PLATFORM)
            message(FATAL_ERROR
                "aeg_add_game_module(${target}) METADATA requires BUILD_ID and TARGET_PLATFORM")
        endif()
        get_filename_component(metadata_file "${ARG_METADATA}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if (ARG_OUTPUT_DIRECTORY)
            get_filename_component(bundle_directory "${ARG_OUTPUT_DIRECTORY}" ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        else()
            set(bundle_directory
                "${CMAKE_CURRENT_BINARY_DIR}/Binaries/Editor/${ARG_TARGET_PLATFORM}/${ARG_BUILD_ID}/$<CONFIG>")
        endif()
        set(bundle_metadata "${bundle_directory}/module.metadata.json")
        set(bundle_manifest "${bundle_directory}/module.manifest.json")
        get_target_property(bundle_name ${target} OUTPUT_NAME)
        if (NOT bundle_name OR bundle_name STREQUAL "bundle_name-NOTFOUND")
            set(bundle_name "${target}")
        endif()
        set(bundle_binary "${bundle_directory}/${bundle_name}${CMAKE_SHARED_LIBRARY_SUFFIX}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${bundle_directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${target}>" "${bundle_binary}"
            COMMAND "$<TARGET_FILE:aeg_module_metadata_tool>"
                --module "${bundle_binary}"
                --metadata "${metadata_file}"
                --output "${bundle_metadata}"
                --manifest "${bundle_manifest}"
                --build-id "${ARG_BUILD_ID}"
                --target "${ARG_TARGET_PLATFORM}"
                --config "$<CONFIG>"
            BYPRODUCTS "${bundle_binary}" "${bundle_metadata}" "${bundle_manifest}"
            VERBATIM)
    endif()
endfunction()

function(aeg_add_game_executable target)
    cmake_parse_arguments(ARG "" "MODULE;APP_SOURCE;BUILD_ID;TARGET_PLATFORM" "SOURCES" ${ARGN})
    if (TARGET ${target})
        message(FATAL_ERROR "aeg_add_game_executable(${target}) would redefine an existing target")
    endif()
    if (ARG_MODULE)
        set(module_target "${ARG_MODULE}")
        if (NOT TARGET ${module_target})
            message(FATAL_ERROR "aeg_add_game_executable(${target}) references missing MODULE ${module_target}")
        endif()
    else()
        set(module_target "${target}Module")
        set(module_sources ${ARG_SOURCES})
        if (ARG_APP_SOURCE)
            # APP_SOURCE is the host entry point and must never be compiled into
            # the shared GameModule as well. Normalize both spellings before
            # removing it so relative template paths are handled consistently.
            get_filename_component(app_source_abs "${ARG_APP_SOURCE}" ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            set(filtered_sources)
            foreach(source IN LISTS module_sources)
                get_filename_component(source_abs "${source}" ABSOLUTE
                    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
                if (NOT source_abs STREQUAL app_source_abs)
                    list(APPEND filtered_sources "${source}")
                endif()
            endforeach()
            set(module_sources ${filtered_sources})
        endif()
        aeg_add_game_module(${module_target} SOURCES ${module_sources})
    endif()
    set(app_sources ${ARG_APP_SOURCE})
    if (NOT app_sources)
        # The convenience form keeps the historical behavior: callers provide
        # a source list that contains the executable entry point.
        set(app_sources ${ARG_SOURCES})
    endif()
    if (NOT app_sources)
        message(FATAL_ERROR "aeg_add_game_executable(${target}) requires APP_SOURCE or SOURCES")
    endif()
    if (ARG_BUILD_ID)
        set(game_build_id "${ARG_BUILD_ID}")
    else()
        set(game_build_id "dev")
    endif()
    if (ARG_TARGET_PLATFORM)
        set(game_target_platform "${ARG_TARGET_PLATFORM}")
    else()
        set(game_target_platform "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if (ARG_MODULE)
        get_target_property(module_sources ${module_target} AEG_GAME_MODULE_SOURCES)
        if (module_sources)
            # A MODULE invocation still produces a self-contained shipping
            # executable.  The shared target is retained for editor loading;
            # the executable compiles the declared module sources directly.
            set(static_entry "${CMAKE_CURRENT_BINARY_DIR}/${target}_static_entry_$<CONFIG>.cpp")
            if (ARG_APP_SOURCE)
                set(static_entry_content
                    "#include <string>\n"
                    "#include \"game_module.hpp\"\n"
                    "#include \"runtime_bootstrap.hpp\"\n"
                    "aeg::module::ModuleDefinition aeg_define_${module_target}()\;\n"
                    "aeg::foundation::Result aeg_configure_game_application(aeg::application::runtime::RuntimeBootRequest&)\;\n"
                    "int main(int argc, char** argv) {\n"
                    "  const aeg::application::runtime::RuntimeBuildIdentity identity{\"${game_build_id}\", \"${game_target_platform}\", \"$<CONFIG>\"}\;\n"
                    "  return aeg::application::runtime::run_game(argc, argv, aeg_define_${module_target}(), identity, &aeg_configure_game_application)\;\n"
                    "}\n")
                string(JOIN "" static_entry_content ${static_entry_content})
                file(GENERATE OUTPUT "${static_entry}" CONTENT "${static_entry_content}")
                list(APPEND app_sources "${static_entry}")
            endif()
            add_executable(${target} ${app_sources} ${module_sources})
            target_link_libraries(${target} PRIVATE AEG::GameRuntime)
            target_include_directories(${target} PRIVATE
                "${AEG_ENGINE_SOURCE_DIR}/Engine/Module/public"
                "${AEG_ENGINE_SOURCE_DIR}/Engine/Pipeline/public")
        else()
            add_executable(${target} ${app_sources})
        target_link_libraries(${target} PRIVATE ${module_target} AEG::GameRuntime)
        endif()
    else()
        # The convenience form owns the source list, so compile those sources
        # again into the shipping executable.  The shared GameModule remains
        # available for the Editor, while the static target has no DLL runtime
        # dependency and retains the explicit module definition symbol.
        if (ARG_APP_SOURCE)
            set(static_entry "${CMAKE_CURRENT_BINARY_DIR}/${target}_static_entry_$<CONFIG>.cpp")
            set(static_entry_content
                "#include \"game_module.hpp\"\n"
                "#include \"runtime_bootstrap.hpp\"\n"
                "aeg::module::ModuleDefinition aeg_define_${module_target}()\;\n"
                "aeg::foundation::Result aeg_configure_game_application(aeg::application::runtime::RuntimeBootRequest&)\;\n"
                "int main(int argc, char** argv) {\n"
                "  const aeg::application::runtime::RuntimeBuildIdentity identity{\"${game_build_id}\", \"${game_target_platform}\", \"$<CONFIG>\"}\;\n"
                "  return aeg::application::runtime::run_game(argc, argv, aeg_define_${module_target}(), identity, &aeg_configure_game_application)\;\n"
                "}\n")
            string(JOIN "" static_entry_content ${static_entry_content})
            file(GENERATE OUTPUT "${static_entry}" CONTENT "${static_entry_content}")
            list(APPEND app_sources "${static_entry}")
        endif()
        add_executable(${target} ${app_sources} ${module_sources})
        target_link_libraries(${target} PRIVATE AEG::GameRuntime)
        target_include_directories(${target} PRIVATE
            "${AEG_ENGINE_SOURCE_DIR}/Engine/Module/public"
            "${AEG_ENGINE_SOURCE_DIR}/Engine/Pipeline/public")
    endif()
    # Keep the Editor module artifact available when consumers build only the
    # shipping target; it is a build-order dependency, never a link dependency.
    add_dependencies(${target} ${module_target})
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()
