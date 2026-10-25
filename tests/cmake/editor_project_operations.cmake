cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS EDITOR FIXTURE_WRITER MODULE_FIXTURE TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
get_filename_component(operation_root "${TEST_ROOT}" ABSOLUTE)
get_filename_component(operation_name "${operation_root}" NAME)
if(NOT operation_name STREQUAL "editor-project-operations")
    message(FATAL_ERROR "Refusing to reset an unexpected operation fixture directory")
endif()
file(REMOVE_RECURSE "${operation_root}")
file(MAKE_DIRECTORY "${operation_root}")
set(ENV{APPDATA} "${operation_root}/Settings")
execute_process(COMMAND "${FIXTURE_WRITER}" --write-editor-operation-fixture "${operation_root}" "${MODULE_FIXTURE}"
    RESULT_VARIABLE written OUTPUT_VARIABLE writer_output ERROR_VARIABLE writer_error TIMEOUT 30)
if(NOT written EQUAL 0)
    message(FATAL_ERROR "Operation fixture writer failed: ${writer_output}\n${writer_error}")
endif()
set(protected_sources
    Project.aegproj Assets/Scenes/Main.aegscene Assets/Scenes/Main.aegscene.meta
    Assets/Features/Main.aegfeature Assets/Features/Main.aegfeature.meta
    ModuleBundle/module.manifest.json)
file(GLOB module_sources RELATIVE "${operation_root}" "${operation_root}/ModuleBundle/*")
list(APPEND protected_sources ${module_sources})
list(REMOVE_DUPLICATES protected_sources)
foreach(source IN LISTS protected_sources)
    file(SHA256 "${operation_root}/${source}" before_${source})
endforeach()

execute_process(COMMAND "${EDITOR}" --project "${operation_root}/Project.aegproj"
    --project-operation cook --module-manifest "${operation_root}/ModuleBundle/module.manifest.json"
    --output-directory "${operation_root}/Cooked" --configuration Debug --frames 240
    WORKING_DIRECTORY "${operation_root}"
    RESULT_VARIABLE cooked OUTPUT_VARIABLE editor_output ERROR_VARIABLE editor_error TIMEOUT 60)
file(WRITE "${operation_root}/main-cook.log" "${editor_output}\n${editor_error}")
if(NOT cooked EQUAL 0 OR NOT EXISTS "${operation_root}/Cooked/current.toml")
    message(FATAL_ERROR "Actual Main did not execute Cook: exit=${cooked}\n${editor_output}\n${editor_error}")
endif()
execute_process(COMMAND "${FIXTURE_WRITER}" --verify-editor-operation-cooked "${operation_root}"
    WORKING_DIRECTORY "${operation_root}"
    RESULT_VARIABLE booted OUTPUT_VARIABLE boot_output ERROR_VARIABLE boot_error TIMEOUT 30)
file(WRITE "${operation_root}/cooked-runtime.log" "${boot_output}\n${boot_error}")
if(NOT booted EQUAL 0)
    message(FATAL_ERROR "Main Cook did not boot/commit the actual DLL Feature: ${boot_output}\n${boot_error}")
endif()
foreach(source IN LISTS protected_sources)
    file(SHA256 "${operation_root}/${source}" after_hash)
    if(NOT "${before_${source}}" STREQUAL "${after_hash}")
        message(FATAL_ERROR "Main operation rewrote a protected input: ${source}")
    endif()
endforeach()

# An explicit operation must reject missing inputs, including before opening a window.
execute_process(COMMAND "${EDITOR}" --project-operation cook --frames 8
    WORKING_DIRECTORY "${operation_root}"
    RESULT_VARIABLE no_project OUTPUT_VARIABLE rejected_output ERROR_VARIABLE rejected_error TIMEOUT 30)
if(NOT "${no_project}" STREQUAL "1" OR
   NOT "${rejected_output}\n${rejected_error}" MATCHES "requires --project")
    message(FATAL_ERROR "Main did not explicitly reject the missing project: ${no_project}\n${rejected_output}\n${rejected_error}")
endif()
file(SHA256 "${operation_root}/Cooked/current.toml" selector_before)
execute_process(COMMAND "${EDITOR}" --project "${operation_root}/Project.aegproj"
    --project-operation cook --output-directory "${operation_root}/Cooked" --frames 240
    WORKING_DIRECTORY "${operation_root}"
    RESULT_VARIABLE no_module OUTPUT_VARIABLE rejected_output ERROR_VARIABLE rejected_error TIMEOUT 60)
if(NOT "${no_module}" STREQUAL "1" OR
   NOT "${rejected_output}\n${rejected_error}" MATCHES "Project operation failed or did not finish:.*[Mm]odule")
    message(FATAL_ERROR "Main did not explicitly reject the missing module: ${no_module}\n${rejected_output}\n${rejected_error}")
endif()
file(SHA256 "${operation_root}/Cooked/current.toml" selector_after)
if(NOT selector_before STREQUAL selector_after)
    message(FATAL_ERROR "Failed Main Cook changed the last-good selector")
endif()
message(STATUS "Actual Main Cook produced a verified Runtime Feature commit; input and last-good guards passed")

# Build a new game Module from source through the actual Main operation. No
# previously compiled DLL or bundle is placed in this project by its writer.
get_filename_component(engine_source "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(compiled_root "${operation_root}/Compiled Project")
execute_process(COMMAND "${FIXTURE_WRITER}" --write-editor-operation-build-fixture "${compiled_root}"
    RESULT_VARIABLE build_written OUTPUT_VARIABLE build_writer_output ERROR_VARIABLE build_writer_error TIMEOUT 30)
if(NOT "${build_written}" STREQUAL "0" OR EXISTS "${compiled_root}/ModuleBundle")
    message(FATAL_ERROR "Source-only Main fixture was not created: ${build_writer_output}\n${build_writer_error}")
endif()
set(build_sources Project.aegproj CMakeLists.txt Source/GameModule.cpp Source/GameModule.metadata.json
    Assets/Scenes/Main.aegscene Assets/Scenes/Main.aegscene.meta
    Assets/Features/Main.aegfeature Assets/Features/Main.aegfeature.meta)
foreach(source IN LISTS build_sources)
    file(SHA256 "${compiled_root}/${source}" build_before_${source})
endforeach()
set(ENV{CMAKE_BUILD_PARALLEL_LEVEL} "4")
execute_process(COMMAND "${EDITOR}" --project "${compiled_root}/Project.aegproj"
    --project-operation build-and-cook --engine-dir "${engine_source}" --cmake "${CMAKE_COMMAND}"
    --target ActualModuleTarget --module-manifest "${compiled_root}/ModuleBundle/module.manifest.json"
    --output-directory "${compiled_root}/Cooked" --configuration Debug --frames 20000
    WORKING_DIRECTORY "${compiled_root}"
    RESULT_VARIABLE built OUTPUT_VARIABLE build_output ERROR_VARIABLE build_error TIMEOUT 360)
file(WRITE "${compiled_root}/main-build-and-cook.log" "${build_output}\n${build_error}")
if(NOT "${built}" STREQUAL "0" OR NOT EXISTS "${compiled_root}/ModuleBundle/module.manifest.json" OR
   NOT EXISTS "${compiled_root}/Cooked/current.toml")
    message(FATAL_ERROR "Main did not build and Cook the new Module: ${built}\n${build_output}\n${build_error}")
endif()
execute_process(COMMAND "${FIXTURE_WRITER}" --verify-editor-operation-cooked "${compiled_root}"
    WORKING_DIRECTORY "${compiled_root}"
    RESULT_VARIABLE compiled_boot OUTPUT_VARIABLE compiled_output ERROR_VARIABLE compiled_error TIMEOUT 30)
file(WRITE "${compiled_root}/cooked-runtime.log" "${compiled_output}\n${compiled_error}")
if(NOT "${compiled_boot}" STREQUAL "0")
    message(FATAL_ERROR "The newly compiled Main Module did not commit in Runtime: ${compiled_output}\n${compiled_error}")
endif()
foreach(source IN LISTS build_sources)
    file(SHA256 "${compiled_root}/${source}" build_after_hash)
    if(NOT "${build_before_${source}}" STREQUAL "${build_after_hash}")
        message(FATAL_ERROR "Main BuildAndCook rewrote authored source: ${source}")
    endif()
endforeach()
message(STATUS "Actual Main built a new source Module, Cooked it, and Runtime committed its Feature")
