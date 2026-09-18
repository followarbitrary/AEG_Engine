cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED ENGINE_DIR OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ENGINE_DIR and TEST_ROOT are required")
endif()

get_filename_component(ENGINE_DIR "${ENGINE_DIR}" ABSOLUTE)
get_filename_component(TEST_ROOT "${TEST_ROOT}" ABSOLUTE)
set(PROJECT_ROOT "${TEST_ROOT}/game-project")
set(BUILD_ROOT "${TEST_ROOT}/game-build")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${PROJECT_ROOT}")
file(COPY "${ENGINE_DIR}/Application/Project/templates/game/" DESTINATION "${PROJECT_ROOT}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_ROOT}" -B "${BUILD_ROOT}"
            "-DENGINE_DIR=${ENGINE_DIR}" -DAEG_BUILD_EDITOR=OFF -DAEG_BUILD_TESTS=OFF -DAEG_BUILD_TOOLS=ON
    RESULT_VARIABLE configure_result OUTPUT_VARIABLE configure_output ERROR_VARIABLE configure_error)
if (NOT configure_result EQUAL 0)
    message(FATAL_ERROR "External game configure failed:\n${configure_output}\n${configure_error}")
endif()

set(SCAFFOLD_ROOT "${TEST_ROOT}/scaffold-output")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug --target aeg_project_scaffold_tool
    RESULT_VARIABLE scaffold_build_result OUTPUT_VARIABLE scaffold_build_output ERROR_VARIABLE scaffold_build_error)
if (NOT scaffold_build_result EQUAL 0)
    message(FATAL_ERROR "Scaffold tool build failed:\n${scaffold_build_output}\n${scaffold_build_error}")
endif()
set(SCAFFOLD_TOOL "${BUILD_ROOT}/aeg-engine/Debug/aeg_project_scaffold_tool.exe")
execute_process(
    COMMAND "${SCAFFOLD_TOOL}" --output "${SCAFFOLD_ROOT}" --name "Outline 示例"
    RESULT_VARIABLE scaffold_result OUTPUT_VARIABLE scaffold_output ERROR_VARIABLE scaffold_error)
if (NOT scaffold_result EQUAL 0)
    message(FATAL_ERROR "External scaffold failed:\n${scaffold_output}\n${scaffold_error}")
endif()
file(COPY "${SCAFFOLD_ROOT}/Assets/Scenes/Main.aegscene" DESTINATION "${PROJECT_ROOT}/Assets/Scenes")

foreach(target IN ITEMS GameModule MyGameExecutable aeg_project_cook_tool)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_ROOT}" --config Debug --target "${target}"
        RESULT_VARIABLE build_result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_error)
    if (NOT build_result EQUAL 0)
        message(FATAL_ERROR "External game target ${target} failed:\n${build_output}\n${build_error}")
    endif()
endforeach()

set(BUNDLE "${PROJECT_ROOT}/Binaries/Editor/windows-x64-vulkan/dev/Debug")
foreach(required IN ITEMS "${BUNDLE}/GameModule.dll" "${BUNDLE}/module.metadata.json" "${BUNDLE}/module.manifest.json")
    if (NOT EXISTS "${required}")
        message(FATAL_ERROR "External game bundle is incomplete: ${required}")
    endif()
endforeach()
file(READ "${BUNDLE}/module.manifest.json" manifest)
string(JSON binary_path GET "${manifest}" binary_path)
string(JSON metadata_path GET "${manifest}" metadata_path)
string(JSON binary_sha256 GET "${manifest}" binary_sha256)
string(JSON metadata_sha256 GET "${manifest}" metadata_sha256)
if (NOT EXISTS "${BUNDLE}/${binary_path}" OR NOT EXISTS "${BUNDLE}/${metadata_path}")
    message(FATAL_ERROR "External game manifest points outside its staged files")
endif()
file(SHA256 "${BUNDLE}/${binary_path}" actual_binary_sha256)
file(SHA256 "${BUNDLE}/${metadata_path}" actual_metadata_sha256)
if (NOT actual_binary_sha256 STREQUAL binary_sha256 OR NOT actual_metadata_sha256 STREQUAL metadata_sha256)
    message(FATAL_ERROR "External game bundle digest verification failed")
endif()
if (NOT EXISTS "${BUILD_ROOT}/Debug/MyGameExecutable.exe")
    message(FATAL_ERROR "External static game executable was not produced")
endif()
set(DELIVERY "${PROJECT_ROOT}/Binaries/Shipping/Debug")
set(COOK_TOOL "${BUILD_ROOT}/aeg-engine/Debug/aeg_project_cook_tool.exe")
execute_process(
    COMMAND "${COOK_TOOL}" --project "${PROJECT_ROOT}/Project.aegproj"
            --module-manifest "${BUNDLE}/module.manifest.json" --output "${DELIVERY}" --config Debug
    RESULT_VARIABLE cook_result OUTPUT_VARIABLE cook_output ERROR_VARIABLE cook_error)
if (NOT cook_result EQUAL 0)
    message(FATAL_ERROR "External project cook failed:\n${cook_output}\n${cook_error}")
endif()
foreach(required IN ITEMS "${DELIVERY}/game.aegbin" "${DELIVERY}/game.manifest" "${DELIVERY}/game.manifest.bin")
    if (NOT EXISTS "${required}")
        message(FATAL_ERROR "Cooked delivery is incomplete: ${required}")
    endif()
endforeach()
set(RUNTIME_TRACE "${TEST_ROOT}/runtime-trace.txt")
execute_process(
    COMMAND "${BUILD_ROOT}/Debug/MyGameExecutable.exe" --manifest "${DELIVERY}/game.manifest"
            --frames 2 --headless --offscreen --trace "${RUNTIME_TRACE}"
    WORKING_DIRECTORY "${TEST_ROOT}" RESULT_VARIABLE run_result OUTPUT_VARIABLE run_output ERROR_VARIABLE run_error)
if (NOT run_result EQUAL 0 OR NOT EXISTS "${RUNTIME_TRACE}")
    message(FATAL_ERROR "Package-only static game launch failed:\n${run_output}\n${run_error}")
endif()
message(STATUS "External game project passed: ${PROJECT_ROOT}")
