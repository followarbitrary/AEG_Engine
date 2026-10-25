include_guard(GLOBAL)

function(aeg_add_game_module target)
    cmake_parse_arguments(ARG "" "METADATA;METADATA_FACTORY_HEADER;METADATA_FACTORY;BUILD_ID;TARGET_PLATFORM;OUTPUT_DIRECTORY" "SOURCES" ${ARGN})
    if (ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "aeg_add_game_module(${target}) received unknown arguments or missing values")
    endif()
    if (TARGET ${target})
        message(FATAL_ERROR "aeg_add_game_module(${target}) would redefine an existing target")
    endif()
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "aeg_add_game_module(${target}) requires SOURCES")
    endif()
    if ((ARG_METADATA_FACTORY_HEADER AND NOT ARG_METADATA_FACTORY) OR
        (ARG_METADATA_FACTORY AND NOT ARG_METADATA_FACTORY_HEADER))
        message(FATAL_ERROR "aeg_add_game_module(${target}) requires both METADATA_FACTORY_HEADER and METADATA_FACTORY")
    endif()
    if (ARG_METADATA AND ARG_METADATA_FACTORY)
        message(FATAL_ERROR "aeg_add_game_module(${target}) METADATA and METADATA_FACTORY are mutually exclusive")
    endif()
    add_library(${target} SHARED ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES AEG_GAME_MODULE_SOURCES "${ARG_SOURCES}")
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_link_libraries(${target} PRIVATE AEG::GameSDK)
    target_include_directories(${target} PRIVATE "${AEG_ENGINE_SOURCE_DIR}/Engine/Module/public")
    target_compile_features(${target} PRIVATE cxx_std_20)

    # Both authoring forms feed the existing canonical bundle publisher. The
    # factory form first exports declarations in a separate host process; it
    # never loads the DLL or runs Feature callbacks to discover contracts.
    if (ARG_METADATA OR ARG_METADATA_FACTORY)
        if (NOT TARGET aeg_module_metadata_tool)
            message(FATAL_ERROR
                "aeg_add_game_module(${target}) metadata publication requires AEG_BUILD_TOOLS=ON "
                "or AEG_BUILD_EDITOR=ON so aeg_module_metadata_tool is available")
        endif()
        if (NOT ARG_BUILD_ID OR NOT ARG_TARGET_PLATFORM)
            message(FATAL_ERROR
                "aeg_add_game_module(${target}) metadata publication requires BUILD_ID and TARGET_PLATFORM")
        endif()
        if (ARG_METADATA_FACTORY)
            if (CMAKE_CROSSCOMPILING)
                message(FATAL_ERROR "aeg_add_game_module(${target}) factory export requires a native host build; use METADATA from a host export when cross-compiling")
            endif()
            if (NOT ARG_METADATA_FACTORY MATCHES "^(::)?[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*$")
                message(FATAL_ERROR "aeg_add_game_module(${target}) METADATA_FACTORY must name a qualified C++ function")
            endif()
            get_filename_component(factory_header "${ARG_METADATA_FACTORY_HEADER}" ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if (NOT EXISTS "${factory_header}")
                message(FATAL_ERROR "aeg_add_game_module(${target}) factory header does not exist: ${factory_header}")
            endif()
            set(exporter_target "${target}_contract_exporter")
            set(metadata_target "${target}_contract_metadata")
            if (TARGET ${exporter_target} OR TARGET ${metadata_target})
                message(FATAL_ERROR "aeg_add_game_module(${target}) metadata helper target already exists")
            endif()
            add_executable(${exporter_target}
                "${AEG_ENGINE_SOURCE_DIR}/tools/feature_module_metadata_exporter.cpp"
                ${ARG_SOURCES})
            # Evaluate these properties at generation time so author include
            # paths, definitions, options and libraries added after this helper
            # are also shared. There is no link from the exporter to the DLL.
            target_include_directories(${exporter_target} PRIVATE
                "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>")
            target_compile_definitions(${exporter_target} PRIVATE
                "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>"
                AEG_FEATURE_CONTRACT_EXPORTER=1
                "AEG_METADATA_FACTORY_HEADER=\"${factory_header}\""
                "AEG_METADATA_FACTORY=${ARG_METADATA_FACTORY}")
            target_compile_options(${exporter_target} PRIVATE
                "$<TARGET_PROPERTY:${target},COMPILE_OPTIONS>")
            target_link_libraries(${exporter_target} PRIVATE
                "$<TARGET_PROPERTY:${target},LINK_LIBRARIES>")
            target_compile_features(${exporter_target} PRIVATE cxx_std_20)
            set(metadata_file
                "${CMAKE_CURRENT_BINARY_DIR}/generated/module-contracts/${target}/$<CONFIG>/module.source.metadata.json")
            add_custom_command(OUTPUT "${metadata_file}"
                COMMAND "$<TARGET_FILE:${exporter_target}>"
                    --output "${metadata_file}"
                    --build-id "${ARG_BUILD_ID}"
                    --target "${ARG_TARGET_PLATFORM}"
                    --config "$<CONFIG>"
                DEPENDS ${exporter_target} "${factory_header}"
                COMMENT "Exporting ${target} contracts from its shared C++ factory"
                VERBATIM)
            add_custom_target(${metadata_target} DEPENDS "${metadata_file}")
            # A typed factory failure stops the build before linking or
            # publishing a replacement DLL and preserves the last-good bundle.
            add_dependencies(${target} ${metadata_target})
        else()
            get_filename_component(metadata_file "${ARG_METADATA}" ABSOLUTE
                BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
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
        # The source metadata is part of the link dependency set so changing
        # only declarations still reruns POST_BUILD and republishes the
        # bundle.  The publisher receives the freshly linked DLL directly and
        # commits it together with metadata and manifest in one transaction.
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${metadata_file}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${bundle_directory}"
            COMMAND "$<TARGET_FILE:aeg_module_metadata_tool>"
                --module "$<TARGET_FILE:${target}>"
                --binary-output "${bundle_binary}"
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
        # The shipping executable loads the verified Feature DLL from its cooked
        # GameBuildManifest.  Module sources belong only to the shared bundle;
        # compiling them into the executable would create a second execution owner.
        if (ARG_APP_SOURCE)
            set(dynamic_entry "${CMAKE_CURRENT_BINARY_DIR}/${target}_dynamic_entry_$<CONFIG>.cpp")
            set(dynamic_entry_content
                "#include \"game_runtime_bootstrap.hpp\"\n"
                "aeg::foundation::Result aeg_configure_game_application(aeg::application::runtime::RuntimeBootRequest&)\;\n"
                "int main(int argc, char** argv) {\n"
                "  const aeg::application::runtime::RuntimeBuildIdentity identity{\"${game_build_id}\", \"${game_target_platform}\", \"$<CONFIG>\"}\;\n"
                "  return aeg::application::runtime::run_game(argc, argv, identity, &aeg_configure_game_application)\;\n"
                "}\n")
            string(JOIN "" dynamic_entry_content ${dynamic_entry_content})
            file(GENERATE OUTPUT "${dynamic_entry}" CONTENT "${dynamic_entry_content}")
            list(APPEND app_sources "${dynamic_entry}")
        endif()
        add_executable(${target} ${app_sources})
        target_link_libraries(${target} PRIVATE AEG::GameRuntime)
        target_include_directories(${target} PRIVATE
            "${AEG_ENGINE_SOURCE_DIR}/Engine/Module/public")
    else()
        add_executable(${target} ${app_sources})
        target_link_libraries(${target} PRIVATE AEG::GameRuntime)
        target_include_directories(${target} PRIVATE
            "${AEG_ENGINE_SOURCE_DIR}/Engine/Module/public")
    endif()
    # Keep the Editor module artifact available when consumers build only the
    # shipping target; it is a build-order dependency, never a link dependency.
    add_dependencies(${target} ${module_target})
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()
