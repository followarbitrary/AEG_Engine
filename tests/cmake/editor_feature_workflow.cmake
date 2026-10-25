cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED EDITOR OR NOT DEFINED FIXTURE_WRITER OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "Editor workflow requires explicit executables and fixture root")
endif()
get_filename_component(root_name "${TEST_ROOT}" NAME)
if(NOT root_name STREQUAL "editor-feature-workflow")
    message(FATAL_ERROR "Unexpected Editor workflow fixture root: ${TEST_ROOT}")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(project_root "${TEST_ROOT}/Source Project")
execute_process(COMMAND "${FIXTURE_WRITER}" --write-editor-operation-build-fixture "${project_root}"
    RESULT_VARIABLE written OUTPUT_VARIABLE writer_output ERROR_VARIABLE writer_error TIMEOUT 30)
if(NOT "${written}" STREQUAL "0" OR EXISTS "${project_root}/ModuleBundle")
    message(FATAL_ERROR "Source-only workflow fixture failed: ${writer_output}\n${writer_error}")
endif()
set(protected_sources Project.aegproj CMakeLists.txt Source/GameModule.cpp Source/GameModule.metadata.json
    Assets/Scenes/Main.aegscene Assets/Scenes/Main.aegscene.meta Assets/Features/Main.aegfeature.meta)
foreach(source IN LISTS protected_sources)
    file(SHA256 "${project_root}/${source}" before_${source})
endforeach()
file(SHA256 "${project_root}/Assets/Features/Main.aegfeature" graph_before)
get_filename_component(engine_source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ENV{CMAKE_BUILD_PARALLEL_LEVEL} "4")
execute_process(COMMAND "${EDITOR}" --project "${project_root}/Project.aegproj"
    --project-operation build-and-cook --engine-dir "${engine_source}" --cmake "${CMAKE_COMMAND}"
    --target ActualModuleTarget --module-manifest "${project_root}/ModuleBundle/module.manifest.json"
    --output-directory "${project_root}/Cooked" --configuration Debug --frames 20000 --verify-feature-workflow
    WORKING_DIRECTORY "${project_root}"
    RESULT_VARIABLE executed OUTPUT_VARIABLE editor_output ERROR_VARIABLE editor_error TIMEOUT 360)
file(WRITE "${project_root}/main-feature-workflow.log" "${editor_output}\n${editor_error}")
if(NOT "${executed}" STREQUAL "0" OR NOT "${editor_output}\n${editor_error}" MATCHES "MAIN_FEATURE_WORKFLOW_COMPLETE")
    message(FATAL_ERROR "Same Main session did not complete the Feature workflow: exit=${executed}\n${editor_output}\n${editor_error}")
endif()
foreach(source IN LISTS protected_sources)
    file(SHA256 "${project_root}/${source}" after_hash)
    if(NOT "${before_${source}}" STREQUAL "${after_hash}")
        message(FATAL_ERROR "Main workflow changed a protected input: ${source}")
    endif()
endforeach()
file(SHA256 "${project_root}/Assets/Features/Main.aegfeature" graph_after)
if(graph_before STREQUAL graph_after)
    message(FATAL_ERROR "Actual Main Save did not persist the authored Process edit")
endif()
# Cook predates the deliberate author Save/Apply; it must remain an independent,
# valid package containing the original graph and the newly compiled DLL.
execute_process(COMMAND "${FIXTURE_WRITER}" --verify-editor-operation-cooked "${project_root}"
    WORKING_DIRECTORY "${project_root}"
    RESULT_VARIABLE booted OUTPUT_VARIABLE boot_output ERROR_VARIABLE boot_error TIMEOUT 30)
file(WRITE "${project_root}/cooked-runtime.log" "${boot_output}\n${boot_error}")
if(NOT "${booted}" STREQUAL "0")
    message(FATAL_ERROR "Workflow Cook package lost its actual DLL commit: ${boot_output}\n${boot_error}")
endif()
message(STATUS "Same Main built/cooked a source DLL, loaded and inspected it, saved and applied its Process edit")
