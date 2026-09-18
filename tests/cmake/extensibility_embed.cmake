cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED ENGINE_DIR OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ENGINE_DIR and TEST_ROOT are required")
endif()

get_filename_component(ENGINE_DIR "${ENGINE_DIR}" ABSOLUTE)
get_filename_component(TEST_ROOT "${TEST_ROOT}" ABSOLUTE)
set(fixture_template "${ENGINE_DIR}/tests/fixtures/extensibility/embed")
set(fixture_source "${TEST_ROOT}/fixture-source")

# TEST_ROOT is owned by this script.  Clearing it avoids reusing a fixture
# cache whose source directory belonged to an earlier test invocation.
file(REMOVE_RECURSE "${TEST_ROOT}")

function(aeg_source_snapshot output)
    execute_process(
        COMMAND git -C "${ENGINE_DIR}" ls-files
        RESULT_VARIABLE git_result OUTPUT_VARIABLE tracked_output ERROR_VARIABLE git_error)
    if (NOT git_result EQUAL 0)
        message(FATAL_ERROR "Unable to list tracked engine files: ${git_error}")
    endif()
    string(REPLACE "\n" ";" tracked_files "${tracked_output}")
    file(GLOB_RECURSE legacy_generated_files LIST_DIRECTORIES FALSE
        "${ENGINE_DIR}/shaders/public/generated/*"
        "${ENGINE_DIR}/shaders/private/generated/*")
    set(snapshot_files ${legacy_generated_files})
    foreach(relative_file IN LISTS tracked_files)
        if (relative_file)
            list(APPEND snapshot_files "${ENGINE_DIR}/${relative_file}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES snapshot_files)
    list(SORT snapshot_files)
    set(snapshot)
    foreach(snapshot_file IN LISTS snapshot_files)
        if (EXISTS "${snapshot_file}")
            file(SHA256 "${snapshot_file}" file_hash)
            string(APPEND snapshot "${snapshot_file}|${file_hash}\n")
        endif()
    endforeach()
    set(${output} "${snapshot}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${fixture_source}")
file(COPY "${fixture_template}/CMakeLists.txt" DESTINATION "${fixture_source}")
file(WRITE "${fixture_source}/consumer.cpp"
    "#include \"runtime_session.hpp\"\nvoid embed_consumer_entry() {}\n")
aeg_source_snapshot(source_snapshot_before)

set(build_one "${TEST_ROOT}/fixture-one")
set(build_two "${TEST_ROOT}/fixture-two")
foreach(build_dir IN ITEMS "${build_one}" "${build_two}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${fixture_source}" -B "${build_dir}"
                "-DENGINE_DIR=${ENGINE_DIR}"
        RESULT_VARIABLE configure_result OUTPUT_VARIABLE configure_output ERROR_VARIABLE configure_error)
    if (NOT configure_result EQUAL 0)
        message(FATAL_ERROR "Embed fixture configure failed in ${build_dir}:\n${configure_output}\n${configure_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_one}" --target embed_consumer
    RESULT_VARIABLE build_result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_error)
if (NOT build_result EQUAL 0)
    message(FATAL_ERROR "Embed fixture build failed:\n${build_output}\n${build_error}")
endif()

# Trigger the real generators in an independently configured consumer tree.
# This intentionally enables the surfaces that own DXC and FlatBuffers, while
# the two fixture trees above remain the runtime-only, no-DXC coverage.
set(generation_build "${TEST_ROOT}/fixture-generation")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${fixture_source}" -B "${generation_build}"
            "-DENGINE_DIR=${ENGINE_DIR}" "-DAEG_EMBED_FIXTURE_GENERATE=ON"
    RESULT_VARIABLE generation_configure_result
    OUTPUT_VARIABLE generation_configure_output ERROR_VARIABLE generation_configure_error)
if (NOT generation_configure_result EQUAL 0)
    message(FATAL_ERROR "Generator fixture configure failed:\n${generation_configure_output}\n${generation_configure_error}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${generation_build}" --target aeg_core_shaders aeg_application_tooling
    RESULT_VARIABLE generation_build_result
    OUTPUT_VARIABLE generation_build_output ERROR_VARIABLE generation_build_error)
if (NOT generation_build_result EQUAL 0)
    message(FATAL_ERROR "Generator fixture build failed:\n${generation_build_output}\n${generation_build_error}")
endif()
file(GLOB generated_headers "${generation_build}/aeg-engine/generated/shaders/include/generated/*.hpp")
file(GLOB generated_sources "${generation_build}/aeg-engine/generated/shaders/src/*.cpp")
file(GLOB generated_flatbuffers "${generation_build}/aeg-engine/generated/asset/*_generated.h")
file(GLOB_RECURSE generated_spirv "${generation_build}/aeg-engine/generated/shaders/*.spv")
file(GLOB_RECURSE generated_shader_packages "${generation_build}/aeg-engine/generated/shaders/*.aegshader")
if (NOT generated_headers OR NOT generated_sources OR NOT generated_flatbuffers OR
    NOT generated_spirv OR NOT generated_shader_packages)
    message(FATAL_ERROR
        "Expected binary-tree shader C++/SPIR-V/package and FlatBuffers outputs were not generated")
endif()
file(REAL_PATH "${generation_build}" generation_build_real)
foreach(generated_file IN LISTS generated_headers generated_sources generated_flatbuffers
        generated_spirv generated_shader_packages)
    file(REAL_PATH "${generated_file}" generated_file_real)
    string(FIND "${generated_file_real}" "${generation_build_real}/" generated_in_binary_tree)
    if (NOT generated_in_binary_tree EQUAL 0)
        message(FATAL_ERROR "Generated file escaped consumer binary tree: ${generated_file_real}")
    endif()
    message(STATUS "Generated binary-tree artifact: ${generated_file_real}")
endforeach()

foreach(build_dir IN ITEMS "${build_one}" "${build_two}")
    get_filename_component(build_dir_absolute "${build_dir}" ABSOLUTE)
    set(engine_binary_dir "${build_dir_absolute}/aeg-engine")
    if (NOT EXISTS "${engine_binary_dir}")
        message(FATAL_ERROR "Engine was not isolated below consumer build tree: ${engine_binary_dir}")
    endif()
    message(STATUS "Fixture engine binary tree: ${engine_binary_dir}")
    message(STATUS "Fixture shader include output: ${engine_binary_dir}/generated/shaders/include/generated")
    message(STATUS "Fixture shader source output: ${engine_binary_dir}/generated/shaders/src")
    message(STATUS "Fixture FlatBuffers output: ${engine_binary_dir}/generated/asset")
endforeach()
if (build_one STREQUAL build_two)
    message(FATAL_ERROR "Fixture consumer builds share a binary directory")
endif()

aeg_source_snapshot(source_snapshot_after)
if (NOT source_snapshot_before STREQUAL source_snapshot_after)
    message(FATAL_ERROR "Engine source tree changed while configuring/building embed fixtures")
endif()
message(STATUS "AEG embed fixture passed: isolated consumer trees and unchanged engine source snapshot")
