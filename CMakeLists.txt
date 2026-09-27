cmake_minimum_required(VERSION 3.25)
project(AEG_Engine LANGUAGES CXX)
enable_language(C)
if (MSVC)
    add_compile_options(/FS)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /FS" CACHE STRING "" FORCE)
endif()

# Standalone engine builds retain the historical developer-friendly defaults.
# Consumers enter through EngineTools.cmake, which supplies OFF defaults before
# adding this directory and therefore does not accidentally grow an editor.
set(AEG_EMBEDDED_DEFAULT OFF)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AEG_EMBEDDED_DEFAULT ON)
endif()
option(AEG_BUILD_EDITOR "Build the AEG editor and editor-only dependencies" ${AEG_EMBEDDED_DEFAULT})
option(AEG_BUILD_TESTS "Build AEG smoke-test executables and register CTest tests" ${AEG_EMBEDDED_DEFAULT})
option(AEG_BUILD_TOOLS "Build AEG content and shader tooling" ${AEG_EMBEDDED_DEFAULT})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/Zc:preprocessor /utf-8)
endif()

option(AEG_RHI_ENABLE_VALIDATION "Compile RHI validation support" ON)
option(AEG_RHI_ENABLE_DEBUG "Compile RHI debug support" ${AEG_RHI_ENABLE_VALIDATION})
option(AEG_RHI_ENABLE_DEBUG_NAMES "Compile RHI object debug-name support" ${AEG_RHI_ENABLE_DEBUG})
option(AEG_VULKAN_ENABLE_VALIDATION "Compile Vulkan validation-layer support" ${AEG_RHI_ENABLE_VALIDATION})
option(AEG_VULKAN_ENABLE_DEBUG_UTILS "Compile Vulkan debug-utils support" ${AEG_RHI_ENABLE_DEBUG})
option(AEG_VULKAN_ENABLE_DEBUG_NAMES "Compile Vulkan object debug-name support" ${AEG_RHI_ENABLE_DEBUG_NAMES})
option(AEG_MATH_ENABLE_SIMD "Enable the x86 SSE2 math implementation when available" ON)
option(AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG
    "Expose the editable Built-in / Editor UI pipeline in an explicit Debug build" ON)

set(AEG_LOG_COMPILE_LEVEL "Auto" CACHE STRING
    "Compile-time minimum log level (Auto, Trace, Debug, Info, Notice, Warning, Error, Critical, Off)")
set_property(CACHE AEG_LOG_COMPILE_LEVEL PROPERTY STRINGS
    Auto Trace Debug Info Notice Warning Error Critical Off)

string(TOUPPER "${AEG_LOG_COMPILE_LEVEL}" AEG_LOG_COMPILE_LEVEL_UPPER)
if (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "AUTO")
    # A generator expression keeps this setting correct for multi-config
    # consumers, where CMAKE_BUILD_TYPE is intentionally empty.
    set(AEG_LOG_COMPILE_LEVEL_VALUE "$<IF:$<CONFIG:Debug>,3,4>")
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "TRACE")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 0)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "DEBUG")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 3)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "INFO")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 4)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "NOTICE")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 5)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "WARNING")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 6)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "ERROR")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 7)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "CRITICAL")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 8)
elseif (AEG_LOG_COMPILE_LEVEL_UPPER STREQUAL "OFF")
    set(AEG_LOG_COMPILE_LEVEL_VALUE 9)
else()
    message(FATAL_ERROR
        "Invalid AEG_LOG_COMPILE_LEVEL='${AEG_LOG_COMPILE_LEVEL}'. "
        "Expected Auto, Trace, Debug, Info, Notice, Warning, Error, Critical, or Off.")
endif()

if (AEG_RHI_ENABLE_DEBUG_NAMES AND NOT AEG_RHI_ENABLE_DEBUG)
    message(FATAL_ERROR
        "AEG_RHI_ENABLE_DEBUG_NAMES requires AEG_RHI_ENABLE_DEBUG=ON")
endif()

if (AEG_VULKAN_ENABLE_DEBUG_NAMES AND NOT AEG_VULKAN_ENABLE_DEBUG_UTILS)
    message(FATAL_ERROR
        "AEG_VULKAN_ENABLE_DEBUG_NAMES requires AEG_VULKAN_ENABLE_DEBUG_UTILS=ON")
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# JSON metadata is part of the engine's offline toolchain.  Keep the check
# close to the dependency declaration so a consumer never silently falls back
# to a machine-installed nlohmann/json version.
set(AEG_NLOHMANN_JSON_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include/nlohmann/json.hpp")
if (NOT EXISTS "${AEG_NLOHMANN_JSON_HEADER}")
    message(FATAL_ERROR
        "Vendored nlohmann/json is missing: ${AEG_NLOHMANN_JSON_HEADER}. "
        "Restore ThirdParty/nlohmann from the checked-in package archive.")
endif()

if (WIN32 AND (AEG_BUILD_EDITOR OR AEG_BUILD_TOOLS))
    set(AEG_DXC_EXECUTABLE "" CACHE FILEPATH
        "Path to the DirectX Shader Compiler executable")

    if (NOT AEG_DXC_EXECUTABLE)
        set(AEG_VULKAN_BIN_DIR "")
        if (Vulkan_GLSLC_EXECUTABLE)
            get_filename_component(AEG_VULKAN_BIN_DIR
                "${Vulkan_GLSLC_EXECUTABLE}" DIRECTORY)
        endif()

        if (AEG_VULKAN_BIN_DIR AND
            EXISTS "${AEG_VULKAN_BIN_DIR}/dxc.exe")
            set(AEG_DXC_EXECUTABLE
                "${AEG_VULKAN_BIN_DIR}/dxc.exe")
        elseif (EXISTS "$ENV{VULKAN_SDK}/Bin/dxc.exe")
            set(AEG_DXC_EXECUTABLE
                "$ENV{VULKAN_SDK}/Bin/dxc.exe")
        endif()

        if (NOT AEG_DXC_EXECUTABLE)
            find_program(AEG_DXC_EXECUTABLE
                NAMES dxc dxc.exe
                HINTS
                    "${AEG_VULKAN_BIN_DIR}"
                    "$ENV{VULKAN_SDK}/Bin"
                    "$ENV{VULKAN_SDK}/Bin/x64"
            )
        endif()

        if (AEG_DXC_EXECUTABLE)
            set(AEG_DXC_EXECUTABLE "${AEG_DXC_EXECUTABLE}"
                CACHE FILEPATH "Path to the DirectX Shader Compiler executable" FORCE)
        endif()
    endif()

    if (NOT AEG_DXC_EXECUTABLE OR
        NOT EXISTS "${AEG_DXC_EXECUTABLE}")
        message(FATAL_ERROR
            "DXC is required to build the HLSL shaders. "
            "Set AEG_DXC_EXECUTABLE to dxc.exe.")
    endif()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/aeg_hlsl.cmake")
endif()

set(AEG_QUILL_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Quill/include")
set(AEG_QUILL_SOURCE_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Quill/src/quill.cc")
set(AEG_QUILL_LICENSE_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Quill/LICENSE")
if (NOT EXISTS "${AEG_QUILL_INCLUDE_DIR}/quill/Backend.h" OR
    NOT EXISTS "${AEG_QUILL_SOURCE_FILE}" OR
    NOT EXISTS "${AEG_QUILL_LICENSE_FILE}")
    message(FATAL_ERROR
        "Vendored Quill source is incomplete. Expected headers, src/quill.cc, and LICENSE under ThirdParty/Quill.")
endif()

# Quill v13 is header-oriented in this checkout. Keep the module interface
# source vendored for provenance, but do not compile it as a C++20 module.
add_library(aeg_quill INTERFACE)
target_include_directories(aeg_quill SYSTEM INTERFACE ${AEG_QUILL_INCLUDE_DIR})
target_compile_definitions(aeg_quill
    INTERFACE
        QUILL_DISABLE_NON_PREFIXED_MACROS=1
        QUILL_COMPILE_ACTIVE_LOG_LEVEL=${AEG_LOG_COMPILE_LEVEL_VALUE}
)

set(AEG_LOGGING_PUBLIC_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Logging/public")

add_library(aeg_logging STATIC
    Engine/Logging/private/logging.cpp
)
target_compile_features(aeg_logging PUBLIC cxx_std_20)
target_include_directories(aeg_logging
    PUBLIC
        ${AEG_LOGGING_PUBLIC_DIR}
)
target_link_libraries(aeg_logging PUBLIC aeg_quill Threads::Threads)

if (MSVC)
    target_compile_options(aeg_logging PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_logging PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_math STATIC
    Engine/Math/private/math.cpp
)
target_compile_features(aeg_math PUBLIC cxx_std_20)
target_compile_definitions(aeg_math PRIVATE AEG_MATH_ENABLE_SIMD=$<BOOL:${AEG_MATH_ENABLE_SIMD}>)
target_include_directories(aeg_math
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Math/public
)

if (MSVC)
    target_compile_options(aeg_math PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_math PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_math_smoke
    tests/math_smoke.cpp
)
target_link_libraries(aeg_math_smoke PRIVATE aeg_math)
endif()

add_library(aeg_foundation STATIC
    Engine/Foundation/private/result.cpp
)
target_compile_features(aeg_foundation PUBLIC cxx_std_20)
target_include_directories(aeg_foundation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Foundation/public
)

if (MSVC)
    target_compile_options(aeg_foundation PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_foundation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_platform INTERFACE)
target_include_directories(aeg_platform
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
)
target_link_libraries(aeg_platform INTERFACE aeg_foundation)

add_library(aeg_input STATIC
    Engine/Input/private/input_system.cpp
)
target_compile_features(aeg_input PUBLIC cxx_std_20)
target_include_directories(aeg_input
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Input/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
)
target_link_libraries(aeg_input PUBLIC aeg_platform)

if (MSVC)
    target_compile_options(aeg_input PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_input PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(ThirdParty/flatbuffers EXCLUDE_FROM_ALL)

set(AEG_COOKED_ASSET_SCHEMA
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/schema/cooked_asset.fbs")
set(AEG_COOKED_ASSET_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/asset")
set(AEG_COOKED_ASSET_GENERATED_HEADER
    "${AEG_COOKED_ASSET_GENERATED_DIR}/cooked_asset_generated.h")
add_custom_command(
    OUTPUT "${AEG_COOKED_ASSET_GENERATED_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${AEG_COOKED_ASSET_GENERATED_DIR}"
    COMMAND $<TARGET_FILE:flatc> --cpp --scoped-enums --gen-object-api
            -o "${AEG_COOKED_ASSET_GENERATED_DIR}" "${AEG_COOKED_ASSET_SCHEMA}"
    DEPENDS flatc "${AEG_COOKED_ASSET_SCHEMA}"
    VERBATIM
)
add_custom_target(aeg_cooked_asset_schema DEPENDS "${AEG_COOKED_ASSET_GENERATED_HEADER}")

add_library(aeg_application_project STATIC
    Application/Project/private/project_context.cpp
    Application/Project/private/project_scaffold.cpp
    Application/Project/private/pipeline_document_io.cpp
)
target_compile_features(aeg_application_project PUBLIC cxx_std_20)
target_include_directories(aeg_application_project
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/toml11/include
)
target_link_libraries(aeg_application_project PUBLIC aeg_foundation aeg_pipeline)
target_compile_definitions(aeg_application_project PRIVATE
    AEG_PROJECT_TEMPLATE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/templates/game")

if (MSVC)
    target_compile_options(aeg_application_project PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_application_project PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_project_smoke
    tests/project_smoke.cpp
)
target_link_libraries(aeg_project_smoke PRIVATE aeg_application_project)

if (MSVC)
    target_compile_options(aeg_project_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_project_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_project_scaffold_smoke
    tests/project_scaffold_smoke.cpp
)
target_link_libraries(aeg_project_scaffold_smoke PRIVATE aeg_application_project aeg_scene)
add_executable(aeg_pipeline_document_io_smoke
    tests/pipeline_document_io_smoke.cpp
)
target_link_libraries(aeg_pipeline_document_io_smoke PRIVATE aeg_application_project)
if (MSVC)
    target_compile_options(aeg_project_scaffold_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_pipeline_document_io_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_project_scaffold_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_pipeline_document_io_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
endif()
endif()

if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
add_library(aeg_editor_composition_model STATIC
    Application/Editor/private/composition/editor_composition_document.cpp
    Application/Editor/private/composition/editor_composition_catalog.cpp
    Application/Editor/private/composition/editor_composition_view.cpp
    Application/Editor/private/composition/editor_composition_commands.cpp
    Application/Editor/private/composition/editor_composition_storage.cpp
    Application/Editor/private/composition/editor_composition_operations.cpp
    Application/Editor/private/composition/editor_composition_preview.cpp
)
target_compile_features(aeg_editor_composition_model PUBLIC cxx_std_20)
target_include_directories(aeg_editor_composition_model
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
           ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public)
target_link_libraries(aeg_editor_composition_model PUBLIC aeg_application_project aeg_pipeline aeg_engine)
if (MSVC)
    target_compile_options(aeg_editor_composition_model PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_composition_model PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_editor_composition_history_smoke tests/editor_composition_history_smoke.cpp)
target_link_libraries(aeg_editor_composition_history_smoke PRIVATE aeg_editor_composition_model)
add_executable(aeg_editor_composition_palette_smoke tests/editor_composition_palette_smoke.cpp)
target_link_libraries(aeg_editor_composition_palette_smoke PRIVATE aeg_editor_composition_model)
add_executable(aeg_editor_composition_view_smoke tests/editor_composition_view_smoke.cpp)
target_link_libraries(aeg_editor_composition_view_smoke PRIVATE aeg_editor_composition_model)
add_executable(aeg_editor_composition_commands_smoke tests/editor_composition_commands_smoke.cpp)
target_link_libraries(aeg_editor_composition_commands_smoke PRIVATE aeg_editor_composition_model)
add_executable(aeg_editor_composition_storage_smoke tests/editor_composition_storage_smoke.cpp)
target_link_libraries(aeg_editor_composition_storage_smoke PRIVATE aeg_editor_composition_model)
add_executable(aeg_editor_composition_operations_smoke tests/editor_composition_operations_smoke.cpp)
target_link_libraries(aeg_editor_composition_operations_smoke PRIVATE aeg_editor_composition_model)
add_executable(aeg_editor_composition_preview_smoke tests/editor_composition_preview_smoke.cpp)
target_link_libraries(aeg_editor_composition_preview_smoke PRIVATE aeg_editor_composition_model)
if (MSVC)
    target_compile_options(aeg_editor_composition_history_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_composition_palette_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_composition_view_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_composition_commands_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_composition_storage_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_composition_operations_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_composition_preview_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_composition_history_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_composition_palette_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_composition_view_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_composition_commands_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_composition_storage_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_composition_operations_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_composition_preview_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
endif()

add_library(aeg_application_tooling STATIC
    Application/Tooling/private/editor_composition_catalog.cpp
    Application/Tooling/private/asset_metadata.cpp
    Application/Tooling/private/asset_package.cpp
    Application/Tooling/private/asset_dependency_packer.cpp
    Application/Tooling/private/asset_cooker.cpp
    Application/Tooling/private/cgltf_mesh_importer.cpp
    Application/Tooling/private/stb_texture_importer.cpp
    Application/Tooling/private/material_asset_importer.cpp
    Application/Tooling/private/font_importer.cpp
    Application/Tooling/private/cooked_asset.cpp
    Application/Tooling/private/cooked_asset_encoder.cpp
    Application/Tooling/private/cooked_artifact_store.cpp
    Application/Tooling/private/project_operation.cpp
    Application/Tooling/private/project_operation_pipeline.cpp
    Application/Tooling/private/project_process.cpp
    Application/Tooling/private/project_cooker.cpp
    Application/Tooling/private/pipeline_asset_encoder.cpp
    Application/Tooling/private/pipeline_asset_importer.cpp
)
target_compile_features(aeg_application_tooling PUBLIC cxx_std_20)
target_include_directories(aeg_application_tooling
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Scene/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Serialization/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/toml11/include
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/stb
        ${AEG_COOKED_ASSET_GENERATED_DIR}
)
target_link_libraries(aeg_application_tooling PUBLIC aeg_foundation aeg_pipeline aeg_asset aeg_module aeg_application_project flatbuffers)
add_dependencies(aeg_application_tooling aeg_cooked_asset_schema)

# The composition operation controller is implemented in the editor model but
# delegates execution to the shared tooling service.  Keep that dependency on
# the model target so every editor composition consumer gets the service
# implementation, including the operations smoke test.
if (TARGET aeg_editor_composition_model)
    target_link_libraries(aeg_editor_composition_model PUBLIC aeg_application_tooling)
endif()

if (MSVC)
    target_compile_options(aeg_application_tooling PRIVATE /W4 /permissive- /FS)
else()
    target_compile_options(aeg_application_tooling PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_asset_metadata_smoke
    tests/asset_metadata_smoke.cpp
)
target_link_libraries(aeg_asset_metadata_smoke PRIVATE aeg_application_tooling)
add_executable(aeg_project_operation_smoke
    tests/project_operation_smoke.cpp
)
target_link_libraries(aeg_project_operation_smoke PRIVATE aeg_application_tooling)
add_executable(aeg_project_cooker_smoke tests/project_cooker_smoke.cpp)
target_link_libraries(aeg_project_cooker_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_asset_metadata_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_project_operation_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_project_cooker_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_asset_metadata_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_project_operation_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_project_cooker_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_asset_package_smoke
    tests/asset_package_smoke.cpp
)
target_include_directories(aeg_asset_package_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
)
target_link_libraries(aeg_asset_package_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_asset_package_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_asset_package_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_cooked_asset_smoke
    tests/cooked_asset_smoke.cpp
)
target_include_directories(aeg_cooked_asset_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
)
target_link_libraries(aeg_cooked_asset_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_cooked_asset_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_cooked_asset_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_cooked_artifact_store_smoke
    tests/cooked_artifact_store_smoke.cpp
)
target_link_libraries(aeg_cooked_artifact_store_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_cooked_artifact_store_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_cooked_artifact_store_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_asset_cooker_smoke
    tests/asset_cooker_smoke.cpp
)
target_link_libraries(aeg_asset_cooker_smoke PRIVATE aeg_application_tooling)

add_executable(aeg_material_asset_importer_smoke
    tests/material_asset_importer_smoke.cpp
)
target_link_libraries(aeg_material_asset_importer_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_asset_cooker_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_asset_cooker_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_stb_texture_importer_smoke
    tests/stb_texture_importer_smoke.cpp
)
target_link_libraries(aeg_stb_texture_importer_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_stb_texture_importer_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_stb_texture_importer_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_runtime_package_closure_smoke
    tests/runtime_package_closure_smoke.cpp
)
target_link_libraries(aeg_runtime_package_closure_smoke PRIVATE aeg_application_tooling)

add_executable(aeg_runtime_task2_contract_smoke tests/runtime_task2_contract_smoke.cpp)
target_link_libraries(aeg_runtime_task2_contract_smoke PRIVATE aeg_application_tooling)

add_executable(aeg_asset_dependency_packer_smoke
    tests/asset_dependency_packer_smoke.cpp
)
target_link_libraries(aeg_asset_dependency_packer_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_asset_dependency_packer_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_asset_dependency_packer_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_cgltf_mesh_importer_smoke
    tests/cgltf_mesh_importer_smoke.cpp
)
target_link_libraries(aeg_cgltf_mesh_importer_smoke PRIVATE aeg_application_tooling)

if (MSVC)
    target_compile_options(aeg_cgltf_mesh_importer_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_cgltf_mesh_importer_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

endif() # AEG_BUILD_TESTS
endif() # AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR

if (AEG_BUILD_TESTS)
add_executable(aeg_input_smoke
    tests/input_system_smoke.cpp
)
target_link_libraries(aeg_input_smoke PRIVATE aeg_input)
if (WIN32 AND (AEG_BUILD_EDITOR OR AEG_BUILD_TOOLS))
    target_compile_definitions(aeg_input_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif()

if (WIN32 AND (AEG_BUILD_EDITOR OR AEG_BUILD_TOOLS))
    enable_language(RC)

    add_library(aeg_application_platform STATIC
        Application/Platform/private/win32/win32_console_interrupt.cpp
        Application/Platform/private/win32/win32_system_fonts.cpp
        Application/Platform/private/win32/win32_startup_splash.cpp
        Application/Platform/private/win32/win32_window.cpp
        Application/Platform/private/win32/win32_window_system.cpp
    )
    target_compile_features(aeg_application_platform PUBLIC cxx_std_20)
    target_include_directories(aeg_application_platform
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/private/win32
    )
    target_compile_definitions(aeg_application_platform
        PRIVATE
            NOMINMAX
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(aeg_application_platform PUBLIC aeg_platform aeg_input user32 gdi32 gdiplus imm32 dwrite ole32 shell32 uuid dwmapi)

    if (MSVC)
        target_compile_options(aeg_application_platform PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aeg_application_platform PRIVATE -Wall -Wextra -Wpedantic)
    endif()

if (AEG_BUILD_TESTS)
    add_executable(aeg_application_platform_win32_smoke
        tests/application_platform_win32_smoke.cpp
        Application/Platform/private/win32/aether_editor_resources.rc
    )
    target_link_libraries(aeg_application_platform_win32_smoke PRIVATE aeg_application_platform)
    target_compile_definitions(aeg_application_platform_win32_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif()

if (AEG_BUILD_EDITOR)
set(AEG_IMGUI_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/ImGui")
if (NOT EXISTS "${AEG_IMGUI_DIR}/imgui.h" OR
    NOT EXISTS "${AEG_IMGUI_DIR}/imgui.cpp" OR
    NOT EXISTS "${AEG_IMGUI_DIR}/imgui_draw.cpp" OR
    NOT EXISTS "${AEG_IMGUI_DIR}/imgui_tables.cpp" OR
    NOT EXISTS "${AEG_IMGUI_DIR}/imgui_widgets.cpp")
    message(FATAL_ERROR
        "Vendored Dear ImGui core source is incomplete under ThirdParty/ImGui.")
endif()

# FreeType is vendored so editor builds remain reproducible and do not depend
# on a machine-installed font library. Disable optional compression/image
# integrations: the ImGui adapter only needs the core outline rasterizer.
set(FT_DISABLE_ZLIB ON CACHE BOOL "Disable FreeType zlib support for AEG" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "Disable FreeType bzip2 support for AEG" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "Disable FreeType PNG support for AEG" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "Disable FreeType HarfBuzz support for AEG" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "Disable FreeType Brotli support for AEG" FORCE)
set(_aeg_build_shared_libs ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(ThirdParty/freetype EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${_aeg_build_shared_libs})
unset(_aeg_build_shared_libs)

add_library(aeg_imgui_core STATIC
    ThirdParty/ImGui/imgui.cpp
    ThirdParty/ImGui/imgui_draw.cpp
    ThirdParty/ImGui/imgui_tables.cpp
    ThirdParty/ImGui/imgui_widgets.cpp
)
target_compile_features(aeg_imgui_core PUBLIC cxx_std_20)
target_include_directories(aeg_imgui_core
    SYSTEM PUBLIC
        ${AEG_IMGUI_DIR}
)
if (MSVC)
    target_compile_options(aeg_imgui_core PRIVATE /W0)
else()
    target_compile_options(aeg_imgui_core PRIVATE -w)
endif()

add_library(aeg_imgui_freetype STATIC
    ThirdParty/ImGui/misc/freetype/imgui_freetype.cpp
)
target_compile_features(aeg_imgui_freetype PUBLIC cxx_std_20)
target_include_directories(aeg_imgui_freetype
    SYSTEM PUBLIC
        ${AEG_IMGUI_DIR}
        ${AEG_IMGUI_DIR}/misc/freetype
)
target_link_libraries(aeg_imgui_freetype PUBLIC aeg_imgui_core freetype)
if (MSVC)
    target_compile_options(aeg_imgui_freetype PRIVATE /W0)
else()
    target_compile_options(aeg_imgui_freetype PRIVATE -w)
endif()

add_library(aeg_editor_framework STATIC
    Application/EditorFramework/private/error/editor_error.cpp
    Application/EditorFramework/private/font/editor_font.cpp
    Application/EditorFramework/private/preferences/editor_preferences.cpp
    Application/EditorFramework/private/imgui/editor_imgui_host.cpp
    Application/EditorFramework/private/imgui/imgui_platform_bridge.cpp
    Application/EditorFramework/private/imgui/imgui_texture_registry.cpp
    Application/EditorFramework/private/workspace/editor_layout.cpp
    Application/EditorFramework/private/workspace/editor_workspace.cpp
)
target_compile_features(aeg_editor_framework PUBLIC cxx_std_20)
target_include_directories(aeg_editor_framework
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/EditorFramework/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Input/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
    PRIVATE
        ${AEG_IMGUI_DIR}
)
target_link_libraries(aeg_editor_framework PUBLIC aeg_foundation aeg_platform aeg_input)
target_link_libraries(aeg_editor_framework PUBLIC aeg_imgui_core aeg_imgui_freetype)

if (MSVC)
    target_compile_options(aeg_editor_framework PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_framework PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Editor font repository and preference persistence stay above the framework
# layer so the UI can consume them without owning filesystem or serialization
# details. The implementation is private to the editor app.
add_library(aeg_editor_font_services STATIC
    Application/Editor/private/font/editor_font_repository.cpp
    Application/Editor/private/preferences/editor_preference_store.cpp
    Application/Editor/private/preferences/editor_preference_controller.cpp
)
target_compile_features(aeg_editor_font_services PUBLIC cxx_std_20)
target_include_directories(aeg_editor_font_services
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/EditorFramework/public
)
target_link_libraries(aeg_editor_font_services PUBLIC aeg_editor_framework)
if (MSVC)
    target_compile_options(aeg_editor_font_services PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_font_services PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_editor_imgui_smoke
    tests/editor_imgui_smoke.cpp
)
target_link_libraries(aeg_editor_imgui_smoke PRIVATE aeg_editor_framework)
target_include_directories(aeg_editor_imgui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
)
if (WIN32)
    target_compile_definitions(aeg_editor_imgui_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_imgui_docking_smoke
    tests/editor_imgui_docking_smoke.cpp
)
target_link_libraries(aeg_editor_imgui_docking_smoke PRIVATE aeg_imgui_core)
if (WIN32)
    target_compile_definitions(aeg_editor_imgui_docking_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_imgui_viewport_smoke
    tests/editor_imgui_viewport_smoke.cpp
)
target_link_libraries(aeg_editor_imgui_viewport_smoke PRIVATE aeg_editor_framework)
if (WIN32)
    target_compile_definitions(aeg_editor_imgui_viewport_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_layout_smoke
    tests/editor_layout_smoke.cpp
)
target_link_libraries(aeg_editor_layout_smoke PRIVATE aeg_editor_framework)
if (WIN32)
    target_compile_definitions(aeg_editor_layout_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_workspace_smoke
    tests/editor_workspace_smoke.cpp
)
target_link_libraries(aeg_editor_workspace_smoke PRIVATE aeg_editor_framework)
if (WIN32)
    target_compile_definitions(aeg_editor_workspace_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_font_validation_smoke
    tests/editor_font_validation_smoke.cpp
)
target_link_libraries(aeg_editor_font_validation_smoke PRIVATE aeg_editor_framework)

if (WIN32 AND (AEG_BUILD_EDITOR OR AEG_BUILD_TOOLS))
    add_executable(aeg_editor_font_smoke
        tests/editor_font_smoke.cpp
        Application/Editor/private/font/editor_system_font_catalog.cpp
    )
    target_include_directories(aeg_editor_font_smoke PRIVATE Application/Editor/private)
    target_link_libraries(aeg_editor_font_smoke PRIVATE aeg_editor_framework aeg_application_platform)
    target_compile_definitions(aeg_editor_font_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

add_executable(aeg_editor_preferences_smoke
    tests/editor_preferences_smoke.cpp
)
target_link_libraries(aeg_editor_preferences_smoke PRIVATE aeg_editor_framework)
if (WIN32)
    target_compile_definitions(aeg_editor_preferences_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_font_repository_smoke
    tests/editor_font_repository_smoke.cpp
)
target_link_libraries(aeg_editor_font_repository_smoke PRIVATE aeg_editor_font_services)
if (WIN32)
    target_compile_definitions(aeg_editor_font_repository_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_preference_store_smoke
    tests/editor_preference_store_smoke.cpp
)
target_link_libraries(aeg_editor_preference_store_smoke PRIVATE aeg_editor_font_services)
if (WIN32)
    target_compile_definitions(aeg_editor_preference_store_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_editor_preference_controller_smoke
    tests/editor_preference_controller_smoke.cpp
)
target_link_libraries(aeg_editor_preference_controller_smoke PRIVATE aeg_editor_font_services)
target_compile_definitions(aeg_editor_preference_controller_smoke PRIVATE
    _CRT_SECURE_NO_WARNINGS
    AEG_ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(aeg_editor_error_smoke
    tests/editor_error_smoke.cpp
)
target_link_libraries(aeg_editor_error_smoke PRIVATE aeg_editor_framework)
if (WIN32)
    target_compile_definitions(aeg_editor_error_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

endif() # AEG_BUILD_TESTS
endif() # AEG_BUILD_EDITOR

if (AEG_BUILD_TESTS)
add_executable(aeg_result_smoke
    tests/result_smoke.cpp
)
target_link_libraries(aeg_result_smoke PRIVATE aeg_foundation)
if (WIN32)
    target_compile_definitions(aeg_result_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_reflection STATIC
    Engine/Reflection/private/reflection.cpp
)
target_compile_features(aeg_reflection PUBLIC cxx_std_20)
target_include_directories(aeg_reflection
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Reflection/public
)
target_link_libraries(aeg_reflection PUBLIC aeg_foundation)

if (MSVC)
    target_compile_options(aeg_reflection PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_reflection PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_reflection_smoke
    tests/reflection_smoke.cpp
)
target_link_libraries(aeg_reflection_smoke PRIVATE aeg_reflection)
if (WIN32)
    target_compile_definitions(aeg_reflection_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_serialization STATIC
    Engine/Serialization/private/serialization.cpp
)
target_compile_features(aeg_serialization PUBLIC cxx_std_20)
target_include_directories(aeg_serialization
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Serialization/public
)
target_link_libraries(aeg_serialization PUBLIC aeg_foundation aeg_reflection)

if (MSVC)
    target_compile_options(aeg_serialization PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_serialization PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_serialization_smoke
    tests/serialization_smoke.cpp
)
target_link_libraries(aeg_serialization_smoke PRIVATE aeg_serialization)
if (WIN32)
    target_compile_definitions(aeg_serialization_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_concurrency STATIC
    Engine/Concurrency/private/concurrency.cpp
)
target_compile_features(aeg_concurrency PUBLIC cxx_std_20)
target_include_directories(aeg_concurrency
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
)
target_link_libraries(aeg_concurrency PUBLIC aeg_foundation Threads::Threads)

if (MSVC)
    target_compile_options(aeg_concurrency PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_concurrency PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_concurrency_smoke
    tests/concurrency_smoke.cpp
)
target_link_libraries(aeg_concurrency_smoke PRIVATE aeg_concurrency)
if (WIN32)
    target_compile_definitions(aeg_concurrency_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_ecs STATIC
    Engine/ECS/private/ecs.cpp
)
target_compile_features(aeg_ecs PUBLIC cxx_std_20)
target_include_directories(aeg_ecs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/ECS/public
)
target_link_libraries(aeg_ecs PUBLIC aeg_foundation aeg_reflection)

if (MSVC)
    target_compile_options(aeg_ecs PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_ecs PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_ecs_smoke
    tests/ecs_smoke.cpp
)
target_link_libraries(aeg_ecs_smoke PRIVATE aeg_ecs)
if (WIN32)
    target_compile_definitions(aeg_ecs_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_pipeline STATIC
    Engine/Pipeline/public/pipeline_builder_backend.hpp
    Engine/Pipeline/private/pipeline.cpp
    Engine/Pipeline/private/pipeline_authoring.cpp
    Engine/Pipeline/private/render_pass.cpp
    Engine/Pipeline/private/pipeline_document.cpp
)
target_compile_features(aeg_pipeline PUBLIC cxx_std_20)
target_include_directories(aeg_pipeline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
)
target_link_libraries(aeg_pipeline PUBLIC aeg_foundation aeg_concurrency Threads::Threads)

if (MSVC)
    target_compile_options(aeg_pipeline PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_asset STATIC
    Engine/Asset/private/asset_identity.cpp
    Engine/Asset/private/cooked_asset.cpp
    Engine/Asset/private/asset_package_reader.cpp
    Engine/Asset/private/asset_handle_table.cpp
    Engine/Asset/private/game_build_manifest.cpp
    Engine/Asset/private/mesh_payload.cpp
    Engine/Asset/private/texture_payload.cpp
    Engine/Asset/private/material_payload.cpp
    Engine/Asset/private/font_payload.cpp
    Engine/Asset/private/scene_payload.cpp
    Engine/Asset/private/pipeline_asset.cpp
)
target_compile_features(aeg_asset PUBLIC cxx_std_20)
target_include_directories(aeg_asset
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
    PRIVATE ${AEG_COOKED_ASSET_GENERATED_DIR}
)
add_dependencies(aeg_asset aeg_cooked_asset_schema)
target_link_libraries(aeg_asset PUBLIC aeg_foundation aeg_pipeline flatbuffers)
if (MSVC)
    target_compile_options(aeg_asset PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_asset PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_pipeline_smoke
    tests/pipeline_smoke.cpp
)
target_link_libraries(aeg_pipeline_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_extension_smoke
    tests/pipeline_extension_smoke.cpp
)
target_link_libraries(aeg_pipeline_extension_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_extension_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_authoring_catalog_smoke
    tests/pipeline_authoring_catalog_smoke.cpp
)
target_link_libraries(aeg_pipeline_authoring_catalog_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_authoring_catalog_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_module_authoring_smoke
    tests/module_authoring_smoke.cpp
)
target_link_libraries(aeg_module_authoring_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_module_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_document_validation_smoke
    tests/pipeline_document_validation_smoke.cpp
)
target_link_libraries(aeg_pipeline_document_validation_smoke PRIVATE aeg_pipeline)
add_executable(aeg_pipeline_asset_codec_smoke
    tests/pipeline_asset_codec_smoke.cpp
)
target_link_libraries(aeg_pipeline_asset_codec_smoke PRIVATE aeg_application_tooling)
add_executable(aeg_runtime_asset_package_smoke
    tests/runtime_asset_package_smoke.cpp
)
target_include_directories(aeg_runtime_asset_package_smoke PRIVATE ${AEG_COOKED_ASSET_GENERATED_DIR})
target_link_libraries(aeg_runtime_asset_package_smoke PRIVATE aeg_asset flatbuffers)
add_executable(aeg_game_build_manifest_smoke tests/game_build_manifest_smoke.cpp)
target_link_libraries(aeg_game_build_manifest_smoke PRIVATE aeg_asset)
add_executable(aeg_cooked_mesh_layout_smoke
    tests/cooked_mesh_layout_smoke.cpp
)
target_link_libraries(aeg_cooked_mesh_layout_smoke PRIVATE aeg_asset)
add_executable(aeg_cooked_scene_dependencies_smoke
    tests/cooked_scene_dependencies_smoke.cpp
)
target_link_libraries(aeg_cooked_scene_dependencies_smoke PRIVATE aeg_asset)
if (WIN32)
    target_compile_definitions(aeg_pipeline_document_validation_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_render_pass_authoring_smoke tests/render_pass_authoring_smoke.cpp)
target_link_libraries(aeg_render_pass_authoring_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_render_pass_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_instance_configuration_smoke tests/pipeline_instance_configuration_smoke.cpp)
target_link_libraries(aeg_pipeline_instance_configuration_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_instance_configuration_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_composition_runtime_smoke tests/pipeline_composition_runtime_smoke.cpp)
target_link_libraries(aeg_pipeline_composition_runtime_smoke PRIVATE aeg_engine)
if (WIN32)
    target_compile_definitions(aeg_pipeline_composition_runtime_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_conformance_smoke
    tests/pipeline_conformance_smoke.cpp
)
target_link_libraries(aeg_pipeline_conformance_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_conformance_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_runtime_smoke
    tests/pipeline_runtime_smoke.cpp
)
target_link_libraries(aeg_pipeline_runtime_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_runtime_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_hot_swap_smoke
    tests/pipeline_hot_swap_smoke.cpp
)
target_link_libraries(aeg_pipeline_hot_swap_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_hot_swap_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_pipeline_phase_scheduler_smoke
    tests/pipeline_phase_scheduler_smoke.cpp
)
target_link_libraries(aeg_pipeline_phase_scheduler_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_phase_scheduler_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_module STATIC
    Engine/Module/private/module.cpp
    Engine/Module/private/module_abi.cpp
    Engine/Module/private/game_module.cpp
    Engine/Module/private/module_bundle.cpp
    Engine/Module/private/module_metadata_json.cpp
    Engine/Module/private/module_context_table.cpp
    Engine/Module/private/dynamic_library.cpp
    Engine/Module/private/dynamic_module.cpp
)
if (AEG_BUILD_TESTS)
    add_executable(aeg_module_abi_header tests/module_abi_header.c)
    add_executable(aeg_module_abi_smoke tests/module_abi_smoke.cpp)
    target_link_libraries(aeg_module_abi_smoke PRIVATE aeg_module)
    add_executable(aeg_module_metadata_smoke tests/module_metadata_smoke.cpp)
    target_link_libraries(aeg_module_metadata_smoke PRIVATE aeg_module)
    add_executable(aeg_module_dynamic_lifetime_smoke tests/module_dynamic_lifetime_smoke.cpp)
    target_link_libraries(aeg_module_dynamic_lifetime_smoke PRIVATE aeg_module)
    add_executable(aeg_module_context_table_smoke tests/module_context_table_smoke.cpp)
    target_link_libraries(aeg_module_context_table_smoke PRIVATE aeg_module)
    if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
        add_executable(aeg_editor_composition_catalog_smoke tests/editor_composition_catalog_smoke.cpp)
        target_link_libraries(aeg_editor_composition_catalog_smoke PRIVATE aeg_application_tooling)
    endif()
endif()
target_compile_features(aeg_module PUBLIC cxx_std_20)
target_include_directories(aeg_module
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
)
target_link_libraries(aeg_module PUBLIC aeg_foundation aeg_pipeline aeg_reflection)

if (MSVC)
    target_compile_options(aeg_module PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_module PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_module_smoke
    tests/module_smoke.cpp
)
target_link_libraries(aeg_module_smoke PRIVATE aeg_module)
if (WIN32)
    target_compile_definitions(aeg_module_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_world STATIC
    Engine/World/private/world_scope.cpp
    Engine/World/private/world_serializer.cpp
)
target_compile_features(aeg_world PUBLIC cxx_std_20)
target_include_directories(aeg_world
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/World/public
)
target_link_libraries(aeg_world PUBLIC aeg_ecs aeg_pipeline aeg_serialization)

if (MSVC)
    target_compile_options(aeg_world PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_world PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_world_scope_smoke
    tests/world_scope_smoke.cpp
)
target_link_libraries(aeg_world_scope_smoke PRIVATE aeg_world)
if (WIN32)
    target_compile_definitions(aeg_world_scope_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_world_serialization_smoke
    tests/world_serialization_smoke.cpp
)
target_link_libraries(aeg_world_serialization_smoke PRIVATE aeg_world)
if (WIN32)
    target_compile_definitions(aeg_world_serialization_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_scene STATIC
    Engine/Scene/private/scene_asset.cpp
)
target_compile_features(aeg_scene PUBLIC cxx_std_20)
target_include_directories(aeg_scene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Scene/public
)
target_link_libraries(aeg_scene PUBLIC aeg_world aeg_serialization)
if (TARGET aeg_application_tooling)
    target_link_libraries(aeg_application_tooling PUBLIC aeg_scene)
endif()

if (MSVC)
    target_compile_options(aeg_scene PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_scene PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_scene_asset_smoke
    tests/scene_asset_smoke.cpp
)
target_link_libraries(aeg_scene_asset_smoke PRIVATE aeg_scene)
if (WIN32)
    target_compile_definitions(aeg_scene_asset_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_world_frame_orchestrator_smoke
    tests/world_frame_orchestrator_smoke.cpp
)
target_link_libraries(aeg_world_frame_orchestrator_smoke PRIVATE aeg_engine aeg_world aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_world_frame_orchestrator_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_world_multi_view_smoke
    tests/world_multi_view_smoke.cpp
)
target_link_libraries(aeg_world_multi_view_smoke PRIVATE aeg_engine aeg_world aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_world_multi_view_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_renderer STATIC
    Engine/Renderer/private/post_process.cpp
    Engine/Renderer/private/light_culling.cpp
    Engine/Renderer/private/shadow_rendering.cpp
    Engine/Renderer/private/shadow_resources.cpp
    Engine/Renderer/private/deferred_lighting_resources.cpp
    Engine/Renderer/private/builtin_render_passes.cpp
    Engine/Renderer/private/render_pipeline_graph.cpp
    Engine/Renderer/private/renderer_authoring.cpp
    Engine/Renderer/private/opaque_render_feature.cpp
    Engine/Renderer/private/renderer.cpp
    Engine/Renderer/private/render_pipeline_variants.cpp
)
target_compile_features(aeg_renderer PUBLIC cxx_std_20)
target_include_directories(aeg_renderer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/public
)
target_link_libraries(aeg_renderer
    PUBLIC
        aeg_ecs
        aeg_math
        aeg_pipeline
    PRIVATE
        aeg_world
        aeg_shader
        aeg_render_graph
)

if (MSVC)
    target_compile_options(aeg_renderer PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_renderer PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

set(AEG_VMA_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/VulkanMemoryAllocator/include")
if (NOT EXISTS "${AEG_VMA_INCLUDE_DIR}/vk_mem_alloc.h")
    message(FATAL_ERROR
        "Vulkan Memory Allocator source is missing: "
        "${AEG_VMA_INCLUDE_DIR}/vk_mem_alloc.h")
endif()

set(AEG_SPIRV_REFLECT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/SPIRV-Reflect")
set(AEG_SPIRV_REFLECT_INCLUDE_DIR
    "${AEG_SPIRV_REFLECT_DIR}/include")
if (NOT EXISTS "${AEG_SPIRV_REFLECT_DIR}/spirv_reflect.h" OR
    NOT EXISTS "${AEG_SPIRV_REFLECT_DIR}/src/spirv_reflect.cpp" OR
    NOT EXISTS "${AEG_SPIRV_REFLECT_INCLUDE_DIR}/spirv/unified1/spirv.h")
    message(FATAL_ERROR
        "Vendored SPIRV-Reflect source is incomplete. Expected headers and source under ThirdParty/SPIRV-Reflect.")
endif()

add_library(aeg_spirv_reflect STATIC
    ThirdParty/SPIRV-Reflect/src/spirv_reflect.cpp
)
target_compile_features(aeg_spirv_reflect PUBLIC cxx_std_20)
target_include_directories(aeg_spirv_reflect
    SYSTEM PUBLIC
        ${AEG_SPIRV_REFLECT_DIR}
        ${AEG_SPIRV_REFLECT_INCLUDE_DIR}
)

if (MSVC)
    target_compile_options(aeg_spirv_reflect PRIVATE /W0)
else()
    target_compile_options(aeg_spirv_reflect PRIVATE -w)
endif()

add_library(aeg_rhi_core STATIC
        Engine/RHI/private/rhi_factory.cpp
)

target_compile_features(aeg_rhi_core PUBLIC cxx_std_20)
target_compile_definitions(aeg_rhi_core
    PUBLIC
        AEG_RHI_ENABLE_VALIDATION=$<BOOL:${AEG_RHI_ENABLE_VALIDATION}>
        AEG_RHI_ENABLE_DEBUG=$<BOOL:${AEG_RHI_ENABLE_DEBUG}>
        AEG_RHI_ENABLE_DEBUG_NAMES=$<BOOL:${AEG_RHI_ENABLE_DEBUG_NAMES}>
)
target_include_directories(aeg_rhi_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
)
target_link_libraries(aeg_rhi_core PUBLIC aeg_foundation)

add_library(aeg_rhi_vulkan STATIC
        Engine/RHI/private/vulkan/vulkan_device.cpp
        Engine/RHI/private/vulkan/vma_impl.cpp
)

target_compile_features(aeg_rhi_vulkan PUBLIC cxx_std_20)
target_compile_definitions(aeg_rhi_vulkan
    PRIVATE
        AEG_VULKAN_ENABLE_VALIDATION=$<BOOL:${AEG_VULKAN_ENABLE_VALIDATION}>
        AEG_VULKAN_ENABLE_DEBUG_UTILS=$<BOOL:${AEG_VULKAN_ENABLE_DEBUG_UTILS}>
        AEG_VULKAN_ENABLE_DEBUG_NAMES=$<BOOL:${AEG_VULKAN_ENABLE_DEBUG_NAMES}>
        VMA_VULKAN_VERSION=1003000
)
if (WIN32)
    target_compile_definitions(aeg_rhi_vulkan PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
endif()
target_include_directories(aeg_rhi_vulkan
    SYSTEM PRIVATE
        ${AEG_VMA_INCLUDE_DIR}
)
target_link_libraries(aeg_rhi_vulkan PUBLIC aeg_foundation aeg_logging Vulkan::Vulkan)

if (MSVC)
    target_compile_options(aeg_rhi_core PRIVATE /W4 /permissive-)
    target_compile_options(aeg_rhi_vulkan PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_rhi_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_rhi_vulkan PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_shader STATIC
    Engine/Shader/private/shader_manager.cpp
)
target_compile_features(aeg_shader PUBLIC cxx_std_20)
target_include_directories(aeg_shader
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shader/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
)
target_link_libraries(aeg_shader PUBLIC aeg_foundation aeg_rhi_core)

if (MSVC)
    target_compile_options(aeg_shader PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_shader PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The runtime RHI links only production backends.
add_library(aeg_rhi INTERFACE)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang" AND
    CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED)
    target_link_libraries(aeg_rhi INTERFACE
        "$<LINK_GROUP:RESCAN,aeg_rhi_core,aeg_rhi_vulkan>")
else()
    target_link_libraries(aeg_rhi INTERFACE aeg_rhi_core aeg_rhi_vulkan)
endif()

add_library(aeg_render_graph STATIC
    Engine/RenderGraph/private/frame_context.cpp
    Engine/RenderGraph/private/render_graph.cpp
    Engine/RenderGraph/private/render_feature_runtime.cpp
    Engine/RenderGraph/private/render_state_flow.cpp
)
target_compile_features(aeg_render_graph PUBLIC cxx_std_20)
target_include_directories(aeg_render_graph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
)
target_link_libraries(aeg_render_graph PUBLIC aeg_foundation aeg_rhi_core aeg_pipeline)

if (MSVC)
    target_compile_options(aeg_render_graph PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_render_graph PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_engine STATIC
    Engine/Runtime/private/engine_frame_orchestrator.cpp
    Engine/Runtime/private/pipeline_composition_runtime.cpp
    Engine/Runtime/private/runtime_components.cpp
    Engine/Runtime/private/runtime_session.cpp
)
target_compile_features(aeg_engine PUBLIC cxx_std_20)
target_include_directories(aeg_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/public
)
target_link_libraries(aeg_engine PUBLIC aeg_foundation aeg_pipeline aeg_render_graph aeg_world aeg_renderer)

if (MSVC)
    target_compile_options(aeg_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Runtime-only package boot and UUID resolver. This target intentionally has no
# dependency on Application/Tooling, TOML, ImGui, or the Editor.
add_library(aeg_application_runtime STATIC
    Application/Runtime/private/runtime_asset_resolver.cpp
    Application/Runtime/private/runtime_asset_cache.cpp
    Application/Runtime/private/runtime_bootstrap.cpp
    Application/Runtime/private/runtime_scene_loader.cpp
)
target_compile_features(aeg_application_runtime PUBLIC cxx_std_20)
target_include_directories(aeg_application_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Resource/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Input/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/abi
)
target_link_libraries(aeg_application_runtime PUBLIC
    aeg_asset aeg_module aeg_scene aeg_engine aeg_renderer aeg_resource aeg_input aeg_shader aeg_rhi)
if (WIN32 AND TARGET aeg_application_platform)
    target_link_libraries(aeg_application_runtime PUBLIC aeg_application_platform)
endif()
if (MSVC)
    target_compile_options(aeg_application_runtime PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_application_runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Public consumer-facing SDK names. The aliases keep game CMake files stable
# while the implementation target names remain private to the engine build.
if (NOT TARGET AEG::GameSDK)
    add_library(aeg_game_sdk INTERFACE)
    target_link_libraries(aeg_game_sdk INTERFACE aeg_module aeg_pipeline)
    add_library(AEG::GameSDK ALIAS aeg_game_sdk)
endif()
if (NOT TARGET AEG::GameRuntime)
    add_library(aeg_game_runtime INTERFACE)
    target_link_libraries(aeg_game_runtime INTERFACE aeg_application_runtime aeg_engine)
    add_library(AEG::GameRuntime ALIAS aeg_game_runtime)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_runtime_frame_clock_smoke tests/runtime_frame_clock_smoke.cpp)
target_include_directories(aeg_runtime_frame_clock_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
target_link_libraries(aeg_runtime_frame_clock_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_game_callbacks_smoke tests/runtime_game_callbacks_smoke.cpp)
target_include_directories(aeg_runtime_game_callbacks_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
target_link_libraries(aeg_runtime_game_callbacks_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_window_host_smoke tests/runtime_window_host_smoke.cpp)
target_include_directories(aeg_runtime_window_host_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
target_link_libraries(aeg_runtime_window_host_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_boot_smoke tests/runtime_boot_smoke.cpp)
target_include_directories(aeg_runtime_boot_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
target_link_libraries(aeg_runtime_boot_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
if (WIN32 AND AEG_BUILD_EDITOR)
    add_executable(aeg_runtime_owned_host_boot_smoke tests/runtime_owned_host_boot_smoke.cpp)
    target_include_directories(aeg_runtime_owned_host_boot_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
    target_link_libraries(aeg_runtime_owned_host_boot_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
    target_compile_definitions(aeg_runtime_owned_host_boot_smoke PRIVATE
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}")
    add_executable(aeg_runtime_authored_scene_readback_smoke tests/runtime_authored_scene_readback_smoke.cpp)
    target_include_directories(aeg_runtime_authored_scene_readback_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
    target_link_libraries(aeg_runtime_authored_scene_readback_smoke PRIVATE
        aeg_application_runtime aeg_application_tooling)
endif()
add_executable(aeg_runtime_scene_boot_smoke tests/runtime_scene_boot_smoke.cpp)
target_link_libraries(aeg_runtime_scene_boot_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_asset_resolution_smoke tests/runtime_asset_resolution_smoke.cpp)
target_link_libraries(aeg_runtime_asset_resolution_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_font_cache_smoke tests/runtime_font_cache_smoke.cpp)
target_link_libraries(aeg_runtime_font_cache_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_shader_contract_smoke tests/shader_contract_smoke.cpp)
target_link_libraries(aeg_shader_contract_smoke PRIVATE aeg_shader)
add_executable(aeg_runtime_material_contract_smoke tests/runtime_material_contract_smoke.cpp)
target_link_libraries(aeg_runtime_material_contract_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_deferred_contract_smoke tests/runtime_deferred_contract_smoke.cpp)
target_link_libraries(aeg_runtime_deferred_contract_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_runtime_renderer_resource_contract_smoke
    tests/runtime_renderer_resource_contract_smoke.cpp)
target_link_libraries(aeg_runtime_renderer_resource_contract_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling)

add_executable(aeg_runtime_object_vulkan_smoke tests/runtime_object_vulkan_smoke.cpp)
target_link_libraries(aeg_runtime_object_vulkan_smoke PRIVATE aeg_application_runtime aeg_renderer aeg_render_graph aeg_rhi aeg_engine)
if (MSVC)
    target_compile_options(aeg_runtime_boot_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_runtime_boot_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_engine_frame_orchestrator_smoke
    tests/engine_frame_orchestrator_smoke.cpp
)
target_link_libraries(aeg_engine_frame_orchestrator_smoke PRIVATE aeg_engine aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_engine_frame_orchestrator_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_runtime_components_scene_smoke
    tests/runtime_components_scene_smoke.cpp
)
target_link_libraries(aeg_runtime_components_scene_smoke PRIVATE aeg_engine aeg_scene)
if (WIN32)
    target_compile_definitions(aeg_runtime_components_scene_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_runtime_component_orchestrator_smoke
    tests/runtime_component_orchestrator_smoke.cpp
)
target_link_libraries(aeg_runtime_component_orchestrator_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_runtime_component_orchestrator_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_smoke
    tests/renderer_smoke.cpp
)
target_link_libraries(aeg_renderer_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_renderer_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_pipeline_variants_smoke
    tests/renderer_pipeline_variants_smoke.cpp
)
target_link_libraries(aeg_renderer_pipeline_variants_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_renderer_pipeline_variants_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_builtin_pass_smoke tests/renderer_builtin_pass_smoke.cpp)
add_executable(aeg_renderer_shadow_view_smoke tests/renderer_shadow_view_smoke.cpp)
target_link_libraries(aeg_renderer_shadow_view_smoke PRIVATE aeg_renderer)
target_link_libraries(aeg_renderer_builtin_pass_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_renderer_builtin_pass_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_authoring_smoke tests/renderer_authoring_smoke.cpp)
add_executable(aeg_renderer_pipeline_graph_smoke tests/renderer_pipeline_graph_smoke.cpp)
target_link_libraries(aeg_renderer_pipeline_graph_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
target_link_libraries(aeg_renderer_authoring_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_renderer_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_render_feature_runtime_smoke tests/render_feature_runtime_smoke.cpp)
add_executable(aeg_frame_retention_smoke tests/frame_retention_smoke.cpp)
target_link_libraries(aeg_frame_retention_smoke PRIVATE aeg_render_graph aeg_rhi)
add_executable(aeg_render_graph_context_contract_smoke tests/render_graph_context_contract_smoke.cpp)
target_link_libraries(aeg_render_graph_context_contract_smoke PRIVATE aeg_render_graph)
add_executable(aeg_render_graph_imported_texture_state_smoke tests/render_graph_imported_texture_state_smoke.cpp)
target_link_libraries(aeg_render_graph_imported_texture_state_smoke PRIVATE aeg_render_graph aeg_rhi)
target_link_libraries(aeg_render_feature_runtime_smoke PRIVATE aeg_render_graph aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_render_feature_runtime_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

set(AEG_RESOURCE_PUBLIC_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Resource/public")

add_library(aeg_resource STATIC
    Engine/Resource/private/material.cpp
    Engine/Resource/private/mesh.cpp
    Engine/Resource/private/render_target.cpp
    Engine/Resource/private/resource_uploader.cpp
    Engine/Resource/private/texture.cpp
    Engine/Resource/private/texture_uploader.cpp
)
target_compile_features(aeg_resource PUBLIC cxx_std_20)
target_include_directories(aeg_resource
    PUBLIC
        ${AEG_RESOURCE_PUBLIC_DIR}
)
target_link_libraries(aeg_resource PUBLIC aeg_foundation aeg_render_graph)

if (MSVC)
    target_compile_options(aeg_resource PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_resource PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_EDITOR)
# The editor import pipeline is intentionally self-contained: image and TOML
# decoding use vendored sources under ThirdParty and never use system codecs.
add_library(aeg_editor_assets STATIC
    Application/Editor/private/editor_asset_pipeline.cpp
    Application/Editor/private/editor_font_pipeline.cpp
)
target_compile_features(aeg_editor_assets PUBLIC cxx_std_20)
target_include_directories(aeg_editor_assets
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/ImGui
    SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/toml11/include
)
target_compile_definitions(aeg_editor_assets PUBLIC
    AEG_ENGINE_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(aeg_editor_assets PUBLIC aeg_foundation aeg_application_project)
if (AEG_BUILD_TESTS)
add_executable(aeg_editor_asset_pipeline_recovery_smoke tests/editor_asset_pipeline_recovery_smoke.cpp)
target_link_libraries(aeg_editor_asset_pipeline_recovery_smoke PRIVATE aeg_editor_assets)
endif()
if (MSVC)
    target_compile_options(aeg_editor_assets PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_assets PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_sources(aeg_editor_framework PRIVATE
    Application/EditorFramework/private/imgui/imgui_renderer.cpp
)
target_include_directories(aeg_editor_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Math/public
)
target_link_libraries(aeg_editor_framework PUBLIC aeg_resource aeg_rhi aeg_shader)
# imgui_renderer.cpp consumes the public math contract directly. Keep the
# dependency explicit so the framework does not rely on aeg_engine/Renderer's
# transitive link order for symbols implemented by Engine/Math.
target_link_libraries(aeg_editor_framework PUBLIC aeg_math)

endif() # AEG_BUILD_EDITOR

if (AEG_BUILD_TESTS)
add_executable(aeg_rhi_smoke
    tests/rhi_smoke.cpp
)
target_link_libraries(aeg_rhi_smoke PRIVATE aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_rhi_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_logging_smoke
    tests/logging_smoke.cpp
)
target_include_directories(aeg_logging_smoke
    PRIVATE
        ${AEG_LOGGING_PUBLIC_DIR}
)
target_link_libraries(aeg_logging_smoke PRIVATE aeg_logging)
if (WIN32)
    target_compile_definitions(aeg_logging_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif()

if (WIN32 AND (AEG_BUILD_EDITOR OR AEG_BUILD_TOOLS))
    add_executable(aeg_project_scaffold_tool
        Application/Tooling/private/project_scaffold_main.cpp
    )
    target_compile_features(aeg_project_scaffold_tool PRIVATE cxx_std_20)
    target_include_directories(aeg_project_scaffold_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/public)
    target_link_libraries(aeg_project_scaffold_tool PRIVATE aeg_application_project)
    if (MSVC)
        target_compile_options(aeg_project_scaffold_tool PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aeg_project_scaffold_tool PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(aeg_project_cook_tool
        Application/Tooling/private/project_cook_main.cpp
    )
    target_compile_features(aeg_project_cook_tool PRIVATE cxx_std_20)
    target_include_directories(aeg_project_cook_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
    target_link_libraries(aeg_project_cook_tool PRIVATE aeg_application_tooling)
    if (MSVC)
        target_compile_options(aeg_project_cook_tool PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aeg_project_cook_tool PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(aeg_module_metadata_tool
        tools/module_metadata_tool.cpp
    )
    target_compile_features(aeg_module_metadata_tool PRIVATE cxx_std_20)
    target_include_directories(aeg_module_metadata_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include)
    target_link_libraries(aeg_module_metadata_tool PRIVATE aeg_module)
    if (MSVC)
        target_compile_options(aeg_module_metadata_tool PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aeg_module_metadata_tool PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(aeg_shader_contract_tool
        tools/shader_contract_tool.cpp
    )
    target_compile_features(aeg_shader_contract_tool PRIVATE cxx_std_20)
    target_include_directories(aeg_shader_contract_tool
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shader/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Foundation/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
    )
    target_link_libraries(aeg_shader_contract_tool PRIVATE aeg_spirv_reflect)
    if (MSVC)
        target_compile_options(aeg_shader_contract_tool PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aeg_shader_contract_tool PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set(AEG_SHADER_OUTPUT_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
    set(AEG_SHADER_PUBLIC_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders/include")
    set(AEG_SHADER_GENERATED_INCLUDE_DIR
        "${AEG_SHADER_PUBLIC_DIR}/generated")
    set(AEG_SHADER_GENERATED_SOURCE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders/src")
    set(AEG_SHADER_NAMESPACE
        aeg::shaders)
    set(AEG_COMMON_SHADER_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")
    set(AEG_CORE_SHADER_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/core")
    set(AEG_TEST_SHADER_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")

if (AEG_BUILD_EDITOR)
    add_library(aeg_core_shaders STATIC)
    target_compile_features(aeg_core_shaders PUBLIC cxx_std_20)
    target_include_directories(aeg_core_shaders
        PUBLIC
            ${AEG_SHADER_PUBLIC_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shader/public
    )
    aeg_add_hlsl_shader_directory(
        TARGET aeg_core_shaders
        SHADER_DIRECTORY "${AEG_CORE_SHADER_SOURCE_DIR}"
        COMMON_SHADER_DIRECTORY "${AEG_COMMON_SHADER_SOURCE_DIR}"
        NAMESPACE "${AEG_SHADER_NAMESPACE}"
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core"
        GENERATED_INCLUDE_DIRECTORY "${AEG_SHADER_GENERATED_INCLUDE_DIR}"
        GENERATED_SOURCE_DIRECTORY "${AEG_SHADER_GENERATED_SOURCE_DIR}"
        MODE CORE
    )
    set(AEG_EDITOR_IMGUI_SHADER_PACKAGE
        "${AEG_SHADER_OUTPUT_DIR}/core_contract/editor_imgui_draw_mesh.aegshader")
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders
        NAME editor_imgui_draw_mesh
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract"
        CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/editor/imgui/draw_mesh.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/editor/imgui/draw_mesh.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_EDITOR_IMGUI_SHADER_PACKAGE
    )
    set(AEG_OPAQUE_UNLIT_SHADER_PACKAGE
        "${AEG_SHADER_OUTPUT_DIR}/core_contract/opaque_unlit_mesh.aegshader")
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders
        NAME opaque_unlit_mesh
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract"
        CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/opaque/unlit_mesh.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/opaque/unlit_mesh.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_OPAQUE_UNLIT_SHADER_PACKAGE
    )
    set(AEG_GBUFFER_MESH_SHADER_PACKAGE
        "${AEG_SHADER_OUTPUT_DIR}/core_contract/gbuffer_mesh.aegshader")
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders
        NAME gbuffer_mesh
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract"
        CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/gbuffer/mesh.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/gbuffer/mesh.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_GBUFFER_MESH_SHADER_PACKAGE
    )
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders
        NAME depth_mesh
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract"
        CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/depth/mesh.vert.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_DEPTH_MESH_SHADER_PACKAGE
    )
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders
        NAME deferred_lighting
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract"
        CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/deferred/lighting.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/deferred/lighting.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_DEFERRED_LIGHTING_SHADER_PACKAGE
    )
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders NAME forward_shadow
        DEPENDENCIES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/shadow/visibility.hlsli"
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract" CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/opaque/unlit_mesh.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/shadow/forward.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_FORWARD_SHADOW_SHADER_PACKAGE)
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders NAME light_culling
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract" CONTRACT_TOOL aeg_shader_contract_tool
        STAGES "compute|cs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/lighting/light_culling.comp.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_LIGHT_CULLING_SHADER_PACKAGE)
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders NAME forward_clustered
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract" CONTRACT_TOOL aeg_shader_contract_tool
        DEPENDENCIES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/shadow/visibility.hlsli"
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/opaque/unlit_mesh.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/lighting/forward_clustered.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_FORWARD_CLUSTERED_SHADER_PACKAGE)
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders NAME deferred_shadow
        DEPENDENCIES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/shadow/visibility.hlsli"
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract" CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/deferred/lighting.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/shadow/deferred.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_DEFERRED_SHADOW_SHADER_PACKAGE)
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders NAME skybox_post_process
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract" CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/deferred/lighting.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/post_process/skybox_post.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_POST_PROCESS_SHADER_PACKAGE)
    if (TARGET aeg_renderer_pipeline_graph_smoke)
        target_compile_definitions(aeg_renderer_pipeline_graph_smoke PRIVATE
            AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}")
        add_dependencies(aeg_renderer_pipeline_graph_smoke aeg_core_shaders)
    endif()
    set(AEG_STATE_COMPOSITE_SHADER_PACKAGE
        "${AEG_SHADER_OUTPUT_DIR}/core_contract/presentation_fullscreen_texture_blit.aegshader")
    aeg_add_hlsl_gameplay_shader_program(
        TARGET aeg_core_shaders
        NAME presentation_fullscreen_texture_blit
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract"
        CONTRACT_TOOL aeg_shader_contract_tool
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/presentation/fullscreen_texture_blit.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/presentation/fullscreen_texture_blit.frag.hlsl"
        PACKAGE_OUTPUT_VARIABLE AEG_STATE_COMPOSITE_SHADER_PACKAGE
    )
    target_compile_definitions(aeg_editor_framework PRIVATE
        AEG_EDITOR_IMGUI_SHADER_PACKAGE_PATH="${AEG_EDITOR_IMGUI_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
    )
    target_compile_definitions(aeg_application_runtime PRIVATE
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}")
    add_dependencies(aeg_application_runtime aeg_core_shaders)
    if (TARGET aeg_runtime_owned_host_boot_smoke)
        add_dependencies(aeg_runtime_owned_host_boot_smoke aeg_core_shaders)
    endif()
    if (TARGET aeg_runtime_authored_scene_readback_smoke)
        add_dependencies(aeg_runtime_authored_scene_readback_smoke aeg_core_shaders)
    endif()
    add_dependencies(aeg_editor_framework aeg_core_shaders)

if (AEG_BUILD_TESTS)
    add_executable(aeg_editor_imgui_renderer_smoke
        tests/editor_imgui_renderer_smoke.cpp
    )
    target_link_libraries(aeg_editor_imgui_renderer_smoke PRIVATE
        aeg_editor_framework
        aeg_renderer
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_imgui_renderer_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )

    add_executable(aeg_opaque_render_feature_smoke
        tests/opaque_render_feature_smoke.cpp
    )
    add_executable(aeg_runtime_material_vulkan_smoke
        tests/runtime_material_vulkan_smoke.cpp
    )
    add_executable(aeg_runtime_lighting_vulkan_smoke
        tests/runtime_lighting_vulkan_smoke.cpp
    )
    target_link_libraries(aeg_runtime_material_vulkan_smoke PRIVATE
        aeg_application_runtime
        aeg_application_tooling
        aeg_resource
        aeg_render_graph
        aeg_rhi
        aeg_shader
        aeg_core_shaders
    )
    target_compile_definitions(aeg_runtime_material_vulkan_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_runtime_material_vulkan_smoke aeg_core_shaders)
    target_link_libraries(aeg_runtime_lighting_vulkan_smoke PRIVATE
        aeg_application_runtime
        aeg_resource
        aeg_render_graph
        aeg_rhi
        aeg_renderer
    )
    add_executable(aeg_renderer_depth_pass_smoke tests/renderer_depth_pass_smoke.cpp)
    add_executable(aeg_renderer_shadow_gpu_smoke tests/renderer_shadow_gpu_smoke.cpp)
    add_executable(aeg_renderer_light_culling_smoke tests/renderer_light_culling_smoke.cpp)
    target_link_libraries(aeg_renderer_light_culling_smoke PRIVATE aeg_renderer aeg_render_graph aeg_shader aeg_rhi aeg_core_shaders)
    target_compile_definitions(aeg_renderer_light_culling_smoke PRIVATE
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}")
    add_dependencies(aeg_renderer_light_culling_smoke aeg_core_shaders)
    target_link_libraries(aeg_renderer_shadow_gpu_smoke PRIVATE aeg_renderer aeg_render_graph aeg_shader aeg_rhi aeg_core_shaders)
    target_compile_definitions(aeg_renderer_shadow_gpu_smoke PRIVATE
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}")
    add_dependencies(aeg_renderer_shadow_gpu_smoke aeg_core_shaders)
    target_link_libraries(aeg_renderer_depth_pass_smoke PRIVATE aeg_renderer aeg_render_graph aeg_shader aeg_rhi aeg_core_shaders)
    target_compile_definitions(aeg_renderer_depth_pass_smoke PRIVATE
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}")
    add_dependencies(aeg_renderer_depth_pass_smoke aeg_core_shaders)
    target_link_libraries(aeg_opaque_render_feature_smoke PRIVATE
        aeg_engine
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_rhi
        aeg_shader
        aeg_core_shaders
    )
    target_compile_definitions(aeg_opaque_render_feature_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_opaque_render_feature_smoke aeg_core_shaders)

    add_executable(aeg_opaque_render_feature_multiframe_smoke
        tests/opaque_render_feature_multiframe_smoke.cpp
    )
    target_link_libraries(aeg_opaque_render_feature_multiframe_smoke PRIVATE
        aeg_engine
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_rhi
        aeg_shader
        aeg_core_shaders
    )
    target_compile_definitions(aeg_opaque_render_feature_multiframe_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_opaque_render_feature_multiframe_smoke aeg_core_shaders)

    add_executable(aeg_opaque_render_feature_uv_depth_smoke
        tests/opaque_render_feature_uv_depth_smoke.cpp
    )
    target_link_libraries(aeg_opaque_render_feature_uv_depth_smoke PRIVATE
        aeg_engine
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_rhi
        aeg_shader
        aeg_core_shaders
    )
    target_compile_definitions(aeg_opaque_render_feature_uv_depth_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_opaque_render_feature_uv_depth_smoke aeg_core_shaders)

    add_executable(aeg_editor_asset_database_smoke
        tests/editor_asset_database_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
    )
    target_include_directories(aeg_editor_asset_database_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_asset_database_smoke PRIVATE aeg_foundation)
    if (WIN32)
        target_compile_definitions(aeg_editor_asset_database_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(aeg_editor_project_smoke
        tests/editor_project_smoke.cpp
        Application/Editor/private/editor_project.cpp
    )
    target_include_directories(aeg_editor_project_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_project_smoke PRIVATE aeg_foundation aeg_application_project)
    if (WIN32)
        target_compile_definitions(aeg_editor_project_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(aeg_editor_project_pipeline_authoring_smoke
        tests/editor_project_pipeline_authoring_smoke.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_project_pipeline_authoring_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_project_pipeline_authoring_smoke PRIVATE
        aeg_foundation
        aeg_pipeline
        aeg_module
        aeg_application_project
        aeg_renderer
    )
    if (WIN32)
        target_compile_definitions(aeg_editor_project_pipeline_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

endif() # AEG_BUILD_TESTS

    add_executable(aeg_editor_application
        Application/Editor/private/editor_application.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
        Application/Platform/private/win32/aether_editor_resources.rc
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
        Application/Editor/private/font/editor_system_font_catalog.cpp
    )
    target_link_libraries(aeg_editor_application PRIVATE
        aeg_application_platform
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_editor_font_services
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_include_directories(aeg_editor_application PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    set_target_properties(aeg_editor_application PROPERTIES OUTPUT_NAME "AetherEngine")
target_compile_definitions(aeg_editor_application PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG=$<AND:$<BOOL:${AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG}>,$<CONFIG:Debug>>
        AEG_EDITOR_IMGUI_SHADER_PACKAGE_PATH="${AEG_EDITOR_IMGUI_SHADER_PACKAGE}"
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_application aeg_core_shaders)
if (AEG_BUILD_TESTS)
    add_executable(aeg_editor_application_identity_smoke
        tests/editor_application_identity_smoke.cpp
    )
    target_compile_definitions(aeg_editor_application_identity_smoke PRIVATE
        AEG_EDITOR_APPLICATION_PATH="$<TARGET_FILE:aeg_editor_application>"
    )
    target_link_libraries(aeg_editor_application_identity_smoke PRIVATE user32)
    add_dependencies(aeg_editor_application_identity_smoke aeg_editor_application)
endif() # AEG_BUILD_TESTS

    add_custom_command(TARGET aeg_editor_application POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:aeg_editor_application>/branding"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Branding/aether-engine-graph-mark.png"
            "$<TARGET_FILE_DIR:aeg_editor_application>/branding/aether-engine-graph-mark.png"
        VERBATIM
    )

    include(GNUInstallDirs)
    install(TARGETS aeg_editor_application
        RUNTIME DESTINATION .
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Fonts"
        DESTINATION .
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/DemoProject"
        DESTINATION .
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Branding/aether-engine-graph-mark.png"
        DESTINATION branding
    )
    install(FILES "${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        DESTINATION shaders/core
        RENAME opaque_unlit_mesh.aegshader
    )
    install(FILES "${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        DESTINATION shaders/core
        RENAME gbuffer_mesh.aegshader
    )
    install(FILES "${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        DESTINATION shaders/core
        RENAME depth_mesh.aegshader
    )
    install(FILES "${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        DESTINATION shaders/core
        RENAME deferred_lighting.aegshader
    )
    install(FILES "${AEG_FORWARD_SHADOW_SHADER_PACKAGE}" "${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        DESTINATION shaders/core)
    install(FILES "${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
        DESTINATION shaders/core
        RENAME presentation_fullscreen_texture_blit.aegshader
    )
    install(FILES "${AEG_EDITOR_IMGUI_SHADER_PACKAGE}"
        DESTINATION shaders/editor
        RENAME editor_imgui_draw_mesh.aegshader
    )
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_NAME "AEG-Engine-Editor")
    set(CPACK_PACKAGE_VERSION "0.1.0")
    include(CPack)

if (AEG_BUILD_TESTS)
    add_executable(aeg_editor_scene_runtime_smoke
        tests/editor_scene_runtime_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_scene_runtime_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_scene_runtime_smoke PRIVATE
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_scene_runtime_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_scene_runtime_smoke aeg_core_shaders)

    get_target_property(AEG_SCENE_TEST_SOURCES aeg_editor_scene_runtime_smoke SOURCES)
    list(REMOVE_ITEM AEG_SCENE_TEST_SOURCES tests/editor_scene_runtime_smoke.cpp)
    get_target_property(AEG_SCENE_TEST_LIBRARIES aeg_editor_scene_runtime_smoke LINK_LIBRARIES)
    get_target_property(AEG_SCENE_TEST_DEFINITIONS aeg_editor_scene_runtime_smoke COMPILE_DEFINITIONS)
    add_executable(aeg_editor_renderer_pipeline_smoke tests/editor_renderer_pipeline_smoke.cpp ${AEG_SCENE_TEST_SOURCES})
    target_include_directories(aeg_editor_renderer_pipeline_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private)
    target_link_libraries(aeg_editor_renderer_pipeline_smoke PRIVATE ${AEG_SCENE_TEST_LIBRARIES})
    target_compile_definitions(aeg_editor_renderer_pipeline_smoke PRIVATE ${AEG_SCENE_TEST_DEFINITIONS})
    add_dependencies(aeg_editor_renderer_pipeline_smoke aeg_core_shaders)

    add_executable(aeg_editor_external_assets_smoke
        tests/editor_external_assets_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_external_assets_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_external_assets_smoke PRIVATE
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_external_assets_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_external_assets_smoke aeg_core_shaders)

    add_executable(aeg_editor_viewport_interaction_smoke
        tests/editor_viewport_interaction_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_viewport_interaction_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_viewport_interaction_smoke PRIVATE
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_viewport_interaction_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_viewport_interaction_smoke aeg_core_shaders)

    add_executable(aeg_editor_history_smoke
        tests/editor_history_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_history_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_history_smoke PRIVATE
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_history_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_history_smoke aeg_core_shaders)

    add_executable(aeg_editor_hierarchy_smoke
        tests/editor_hierarchy_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_hierarchy_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_hierarchy_smoke PRIVATE
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_hierarchy_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_hierarchy_smoke aeg_core_shaders)

    add_executable(aeg_editor_inspector_smoke
        tests/editor_inspector_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_project_pipeline_authoring.cpp
    )
    target_include_directories(aeg_editor_inspector_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    )
    target_link_libraries(aeg_editor_inspector_smoke PRIVATE
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_inspector_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_inspector_smoke aeg_core_shaders)

    add_executable(aeg_triangle
        tests/triangle.cpp
    )
    aeg_add_hlsl_shader_directory(
        TARGET aeg_triangle
        SHADER_DIRECTORY "${AEG_TEST_SHADER_SOURCE_DIR}"
        COMMON_SHADER_DIRECTORY "${AEG_COMMON_SHADER_SOURCE_DIR}"
        NAMESPACE "${AEG_SHADER_NAMESPACE}"
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/gameplay"
        GENERATED_INCLUDE_DIRECTORY "${AEG_SHADER_GENERATED_INCLUDE_DIR}"
        GENERATED_SOURCE_DIRECTORY "${AEG_SHADER_GENERATED_SOURCE_DIR}"
        MODE GAMEPLAY
        CONTRACT_TOOL aeg_shader_contract_tool
        PACKAGE_OUTPUT_VARIABLE AEG_TRIANGLE_SHADER_PACKAGE
    )
    target_link_libraries(aeg_triangle PRIVATE
        aeg_rhi
        aeg_math
        aeg_resource
        aeg_shader
        aeg_core_shaders
        user32
        gdi32
    )
    target_compile_definitions(aeg_triangle PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_TRIANGLE_SHADER_PACKAGE_PATH="${AEG_TRIANGLE_SHADER_PACKAGE}"
    )
endif() # AEG_BUILD_TESTS
endif() # AEG_BUILD_EDITOR
endif() # WIN32 AND (AEG_BUILD_EDITOR OR AEG_BUILD_TOOLS)

if (BUILD_TESTING AND AEG_BUILD_TESTS)
    add_test(NAME aeg_extensibility_embed
        COMMAND "${CMAKE_COMMAND}"
            "-DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/extensibility-embed"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/extensibility_embed.cmake")
    # These integration tests configure and compile independent cold build trees.
    # A smoke-test timeout is insufficient for their build/cook/launch coverage.
    set_tests_properties(aeg_extensibility_embed PROPERTIES TIMEOUT 600)
    if (AEG_BUILD_TOOLS)
        add_test(NAME aeg_extensibility_external_project
            COMMAND "${CMAKE_COMMAND}"
                "-DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/extensibility-external-示例"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/extensibility_external_project.cmake")
        set_tests_properties(aeg_extensibility_external_project PROPERTIES TIMEOUT 600)
        add_test(NAME aeg_game_project_runtime
            COMMAND "${CMAKE_COMMAND}"
                "-DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/game-project-runtime"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/game_project_runtime.cmake")
        set_tests_properties(aeg_game_project_runtime PROPERTIES TIMEOUT 900)
    endif()
    add_test(NAME aeg_math_smoke COMMAND aeg_math_smoke)
    add_test(NAME aeg_result_smoke COMMAND aeg_result_smoke)
    add_test(NAME aeg_project_smoke COMMAND aeg_project_smoke)
    add_test(NAME aeg_project_scaffold_smoke COMMAND aeg_project_scaffold_smoke)
    add_test(NAME aeg_pipeline_document_io_smoke COMMAND aeg_pipeline_document_io_smoke)
    if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
    add_test(NAME aeg_asset_metadata_smoke COMMAND aeg_asset_metadata_smoke)
    add_test(NAME aeg_project_operation_smoke COMMAND aeg_project_operation_smoke)
    add_test(NAME aeg_project_cooker_smoke COMMAND aeg_project_cooker_smoke)
    add_test(NAME aeg_runtime_asset_package_smoke COMMAND aeg_runtime_asset_package_smoke)
    add_test(NAME aeg_runtime_asset_resolution_smoke COMMAND aeg_runtime_asset_resolution_smoke)
    add_test(NAME aeg_runtime_font_cache_smoke COMMAND aeg_runtime_font_cache_smoke)
    add_test(NAME aeg_shader_contract_smoke COMMAND aeg_shader_contract_smoke)
    add_test(NAME aeg_runtime_material_contract_smoke COMMAND aeg_runtime_material_contract_smoke)
    add_test(NAME aeg_runtime_deferred_contract_smoke COMMAND aeg_runtime_deferred_contract_smoke)
    add_test(NAME aeg_runtime_renderer_resource_contract_smoke
        COMMAND aeg_runtime_renderer_resource_contract_smoke)
    add_test(NAME aeg_runtime_object_vulkan_smoke COMMAND aeg_runtime_object_vulkan_smoke)
    add_test(NAME aeg_game_build_manifest_smoke COMMAND aeg_game_build_manifest_smoke)
    add_test(NAME aeg_runtime_boot_smoke COMMAND aeg_runtime_boot_smoke)
    add_test(NAME aeg_runtime_scene_boot_smoke COMMAND aeg_runtime_scene_boot_smoke)
    add_test(NAME aeg_runtime_window_host_smoke COMMAND aeg_runtime_window_host_smoke)
    if (WIN32 AND AEG_BUILD_EDITOR)
        add_test(NAME aeg_runtime_owned_host_boot_smoke COMMAND aeg_runtime_owned_host_boot_smoke)
        set_tests_properties(aeg_runtime_owned_host_boot_smoke PROPERTIES TIMEOUT 120)
        add_test(NAME aeg_runtime_authored_scene_readback_smoke COMMAND aeg_runtime_authored_scene_readback_smoke)
        set_tests_properties(aeg_runtime_authored_scene_readback_smoke PROPERTIES TIMEOUT 180)
    endif()
    add_test(NAME aeg_cooked_mesh_layout_smoke COMMAND aeg_cooked_mesh_layout_smoke)
    add_test(NAME aeg_cooked_scene_dependencies_smoke COMMAND aeg_cooked_scene_dependencies_smoke)
    add_test(NAME aeg_asset_package_smoke COMMAND aeg_asset_package_smoke)
    add_test(NAME aeg_cooked_asset_smoke COMMAND aeg_cooked_asset_smoke)
    add_test(NAME aeg_cooked_artifact_store_smoke COMMAND aeg_cooked_artifact_store_smoke)
    add_test(NAME aeg_asset_cooker_smoke COMMAND aeg_asset_cooker_smoke)
    add_test(NAME aeg_material_asset_importer_smoke COMMAND aeg_material_asset_importer_smoke)
    add_test(NAME aeg_stb_texture_importer_smoke COMMAND aeg_stb_texture_importer_smoke)
    add_test(NAME aeg_asset_dependency_packer_smoke COMMAND aeg_asset_dependency_packer_smoke)
    add_test(NAME aeg_runtime_package_closure_smoke COMMAND aeg_runtime_package_closure_smoke)
    add_test(NAME aeg_runtime_task2_contract_smoke COMMAND aeg_runtime_task2_contract_smoke)
    add_test(NAME aeg_cgltf_mesh_importer_smoke COMMAND aeg_cgltf_mesh_importer_smoke)
    endif()
    add_test(NAME aeg_input_smoke COMMAND aeg_input_smoke)
    if (AEG_BUILD_EDITOR)
    add_test(NAME aeg_editor_imgui_smoke COMMAND aeg_editor_imgui_smoke)
    add_test(NAME aeg_editor_imgui_docking_smoke COMMAND aeg_editor_imgui_docking_smoke)
    add_test(NAME aeg_editor_imgui_viewport_smoke COMMAND aeg_editor_imgui_viewport_smoke)
    add_test(NAME aeg_editor_layout_smoke COMMAND aeg_editor_layout_smoke)
    add_test(NAME aeg_editor_workspace_smoke COMMAND aeg_editor_workspace_smoke)
    add_test(NAME aeg_editor_preferences_smoke COMMAND aeg_editor_preferences_smoke)
    add_test(NAME aeg_editor_font_repository_smoke COMMAND aeg_editor_font_repository_smoke)
    add_test(NAME aeg_editor_preference_store_smoke COMMAND aeg_editor_preference_store_smoke)
    add_test(NAME aeg_editor_preference_controller_smoke COMMAND aeg_editor_preference_controller_smoke)
    add_test(NAME aeg_editor_error_smoke COMMAND aeg_editor_error_smoke)
    add_test(NAME aeg_editor_asset_pipeline_recovery_smoke COMMAND aeg_editor_asset_pipeline_recovery_smoke)
    add_test(NAME aeg_editor_font_validation_smoke COMMAND aeg_editor_font_validation_smoke)
    if (WIN32)
        add_test(NAME aeg_editor_font_smoke COMMAND aeg_editor_font_smoke)
        add_test(NAME aeg_application_platform_win32_smoke COMMAND aeg_application_platform_win32_smoke)
    endif()
    endif()
    add_test(NAME aeg_reflection_smoke COMMAND aeg_reflection_smoke)
    add_test(NAME aeg_serialization_smoke COMMAND aeg_serialization_smoke)
    add_test(NAME aeg_concurrency_smoke COMMAND aeg_concurrency_smoke)
    add_test(NAME aeg_ecs_smoke COMMAND aeg_ecs_smoke)
    add_test(NAME aeg_pipeline_smoke COMMAND aeg_pipeline_smoke)
    add_test(NAME aeg_pipeline_extension_smoke COMMAND aeg_pipeline_extension_smoke)
    add_test(NAME aeg_pipeline_authoring_catalog_smoke COMMAND aeg_pipeline_authoring_catalog_smoke)
    add_test(NAME aeg_module_authoring_smoke COMMAND aeg_module_authoring_smoke)
    add_test(NAME aeg_pipeline_document_validation_smoke COMMAND aeg_pipeline_document_validation_smoke)
    add_test(NAME aeg_pipeline_asset_codec_smoke COMMAND aeg_pipeline_asset_codec_smoke)
    add_test(NAME aeg_render_pass_authoring_smoke COMMAND aeg_render_pass_authoring_smoke)
    add_test(NAME aeg_pipeline_instance_configuration_smoke COMMAND aeg_pipeline_instance_configuration_smoke)
    add_test(NAME aeg_pipeline_composition_runtime_smoke COMMAND aeg_pipeline_composition_runtime_smoke)
    add_test(NAME aeg_pipeline_conformance_smoke COMMAND aeg_pipeline_conformance_smoke)
    add_test(NAME aeg_pipeline_runtime_smoke COMMAND aeg_pipeline_runtime_smoke)
    add_test(NAME aeg_pipeline_hot_swap_smoke COMMAND aeg_pipeline_hot_swap_smoke)
    add_test(NAME aeg_pipeline_phase_scheduler_smoke COMMAND aeg_pipeline_phase_scheduler_smoke)
    add_test(NAME aeg_module_smoke COMMAND aeg_module_smoke)
    add_test(NAME aeg_module_abi_header COMMAND aeg_module_abi_header)
    add_test(NAME aeg_module_abi_smoke COMMAND aeg_module_abi_smoke)
    add_test(NAME aeg_module_metadata_smoke COMMAND aeg_module_metadata_smoke)
    add_test(NAME aeg_module_dynamic_lifetime_smoke COMMAND aeg_module_dynamic_lifetime_smoke)
    add_test(NAME aeg_module_context_table_smoke COMMAND aeg_module_context_table_smoke)
    if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
        add_test(NAME aeg_editor_composition_catalog_smoke COMMAND aeg_editor_composition_catalog_smoke)
        add_test(NAME aeg_editor_composition_history_smoke COMMAND aeg_editor_composition_history_smoke)
        add_test(NAME aeg_editor_composition_palette_smoke COMMAND aeg_editor_composition_palette_smoke)
        add_test(NAME aeg_editor_composition_view_smoke COMMAND aeg_editor_composition_view_smoke)
        add_test(NAME aeg_editor_composition_commands_smoke COMMAND aeg_editor_composition_commands_smoke)
        add_test(NAME aeg_editor_composition_storage_smoke COMMAND aeg_editor_composition_storage_smoke)
        add_test(NAME aeg_editor_composition_operations_smoke COMMAND aeg_editor_composition_operations_smoke)
        add_test(NAME aeg_editor_composition_preview_smoke COMMAND aeg_editor_composition_preview_smoke)
    endif()
    add_test(NAME aeg_world_scope_smoke COMMAND aeg_world_scope_smoke)
    add_test(NAME aeg_world_serialization_smoke COMMAND aeg_world_serialization_smoke)
    add_test(NAME aeg_scene_asset_smoke COMMAND aeg_scene_asset_smoke)
    add_test(NAME aeg_world_frame_orchestrator_smoke COMMAND aeg_world_frame_orchestrator_smoke)
    add_test(NAME aeg_world_multi_view_smoke COMMAND aeg_world_multi_view_smoke)
    add_test(NAME aeg_engine_frame_orchestrator_smoke COMMAND aeg_engine_frame_orchestrator_smoke)
    add_test(NAME aeg_runtime_components_scene_smoke COMMAND aeg_runtime_components_scene_smoke)
    add_test(NAME aeg_runtime_component_orchestrator_smoke COMMAND aeg_runtime_component_orchestrator_smoke)
    add_test(NAME aeg_renderer_smoke COMMAND aeg_renderer_smoke)
    add_test(NAME aeg_renderer_pipeline_variants_smoke COMMAND aeg_renderer_pipeline_variants_smoke)
    add_test(NAME aeg_renderer_builtin_pass_smoke COMMAND aeg_renderer_builtin_pass_smoke)
    add_test(NAME aeg_renderer_shadow_view_smoke COMMAND aeg_renderer_shadow_view_smoke)
    add_test(NAME aeg_renderer_authoring_smoke COMMAND aeg_renderer_authoring_smoke)
    add_test(NAME aeg_renderer_pipeline_graph_smoke COMMAND aeg_renderer_pipeline_graph_smoke)
    set_tests_properties(aeg_renderer_pipeline_graph_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME aeg_render_feature_runtime_smoke COMMAND aeg_render_feature_runtime_smoke)
    add_test(NAME aeg_frame_retention_smoke COMMAND aeg_frame_retention_smoke)
    add_test(NAME aeg_render_graph_context_contract_smoke COMMAND aeg_render_graph_context_contract_smoke)
    add_test(NAME aeg_render_graph_imported_texture_state_smoke COMMAND aeg_render_graph_imported_texture_state_smoke)
    add_test(NAME aeg_rhi_smoke COMMAND aeg_rhi_smoke)
    add_test(NAME aeg_logging_smoke COMMAND aeg_logging_smoke)
    if (WIN32 AND AEG_BUILD_EDITOR)
        add_test(NAME aeg_triangle COMMAND aeg_triangle --frames 1)
    endif()
    if (WIN32 AND AEG_BUILD_EDITOR)
        add_test(NAME aeg_editor_imgui_renderer_smoke COMMAND aeg_editor_imgui_renderer_smoke)
        add_test(NAME aeg_opaque_render_feature_smoke COMMAND aeg_opaque_render_feature_smoke)
        add_test(NAME aeg_runtime_material_vulkan_smoke COMMAND aeg_runtime_material_vulkan_smoke)
        add_test(NAME aeg_runtime_lighting_vulkan_smoke COMMAND aeg_runtime_lighting_vulkan_smoke)
        add_test(NAME aeg_forward_render_feature_smoke COMMAND aeg_opaque_render_feature_smoke --forward)
        add_test(NAME aeg_deferred_render_feature_smoke COMMAND aeg_opaque_render_feature_smoke --deferred)
        add_test(NAME aeg_deferred_backlit_smoke COMMAND aeg_opaque_render_feature_smoke --deferred-backlit)
        set_tests_properties(aeg_deferred_render_feature_smoke aeg_deferred_backlit_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_forward_render_feature_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_renderer_depth_pass_smoke COMMAND aeg_renderer_depth_pass_smoke)
        add_test(NAME aeg_forward_shadow_smoke COMMAND aeg_renderer_shadow_gpu_smoke)
        add_test(NAME aeg_renderer_light_culling_smoke COMMAND aeg_renderer_light_culling_smoke)
        set_tests_properties(aeg_renderer_light_culling_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_deferred_shadow_smoke COMMAND aeg_renderer_shadow_gpu_smoke --deferred)
        add_test(NAME aeg_forward_local_lights_smoke COMMAND aeg_renderer_shadow_gpu_smoke --local-lights)
        add_test(NAME aeg_forward_post_process_smoke COMMAND aeg_renderer_shadow_gpu_smoke --post-process)
        add_test(NAME aeg_deferred_post_process_smoke COMMAND aeg_renderer_shadow_gpu_smoke --deferred-post-process)
        set_tests_properties(aeg_forward_post_process_smoke aeg_deferred_post_process_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_forward_local_lights_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_forward_shadow_smoke aeg_deferred_shadow_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_renderer_depth_pass_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_opaque_render_feature_multiframe_smoke COMMAND aeg_opaque_render_feature_multiframe_smoke)
        add_test(NAME aeg_opaque_render_feature_uv_depth_smoke COMMAND aeg_opaque_render_feature_uv_depth_smoke)
        add_test(NAME aeg_editor_scene_runtime_smoke COMMAND aeg_editor_scene_runtime_smoke)
        add_test(NAME aeg_editor_renderer_pipeline_smoke COMMAND aeg_editor_renderer_pipeline_smoke)
        set_tests_properties(aeg_editor_renderer_pipeline_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_editor_project_pipeline_authoring_smoke COMMAND aeg_editor_project_pipeline_authoring_smoke)
        add_test(NAME aeg_editor_external_assets_smoke COMMAND aeg_editor_external_assets_smoke)
        add_test(NAME aeg_editor_viewport_interaction_smoke COMMAND aeg_editor_viewport_interaction_smoke)
        add_test(NAME aeg_editor_history_smoke COMMAND aeg_editor_history_smoke)
        add_test(NAME aeg_editor_hierarchy_smoke COMMAND aeg_editor_hierarchy_smoke)
        add_test(NAME aeg_editor_inspector_smoke COMMAND aeg_editor_inspector_smoke)
        add_test(NAME aeg_editor_asset_database_smoke COMMAND aeg_editor_asset_database_smoke)
        add_test(NAME aeg_editor_project_smoke COMMAND aeg_editor_project_smoke)
        add_test(NAME aeg_editor_application_identity_smoke COMMAND aeg_editor_application_identity_smoke)
        add_test(NAME aeg_editor_application COMMAND aeg_editor_application --frames 60 --stress-resize)
        add_test(NAME aeg_editor_application_preferences
                 COMMAND aeg_editor_application --frames 60 --stress-resize --open-preferences)
        add_test(NAME aeg_editor_application_native_viewports
                 COMMAND aeg_editor_application --frames 60 --stress-native-viewports)
        set_tests_properties(aeg_editor_application_native_viewports PROPERTIES TIMEOUT 30)
        add_test(NAME aeg_editor_application_pipeline_recovery
            COMMAND "${CMAKE_COMMAND}"
                "-DEDITOR=$<TARGET_FILE:aeg_editor_application>"
                "-DFIXTURE_WRITER=$<TARGET_FILE:aeg_editor_renderer_pipeline_smoke>"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/editor-pipeline-recovery"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/editor_pipeline_recovery.cmake")
        set_tests_properties(aeg_editor_application_pipeline_recovery PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        add_test(NAME aeg_editor_application_missing_project
                 COMMAND aeg_editor_application --frames 2 --project "${CMAKE_CURRENT_BINARY_DIR}/missing-recovery-project/project.aegproject")
        set_tests_properties(aeg_editor_imgui_renderer_smoke aeg_opaque_render_feature_smoke
                aeg_opaque_render_feature_multiframe_smoke aeg_opaque_render_feature_uv_depth_smoke
                aeg_editor_scene_runtime_smoke aeg_editor_external_assets_smoke aeg_editor_viewport_interaction_smoke
                aeg_editor_history_smoke aeg_editor_hierarchy_smoke aeg_editor_inspector_smoke
                aeg_editor_asset_database_smoke aeg_editor_project_smoke aeg_editor_application
                aeg_editor_application_preferences
                aeg_editor_application_native_viewports
            PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]"
        )
    endif()
    if (MINGW)
        get_filename_component(AEG_MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        function(aeg_set_mingw_runtime_for_registered_tests)
            foreach(aeg_test_name IN LISTS ARGN)
                if (TEST "${aeg_test_name}")
                    set_tests_properties("${aeg_test_name}" PROPERTIES
                        ENVIRONMENT "PATH=${AEG_MINGW_RUNTIME_DIR};$ENV{PATH}")
                endif()
            endforeach()
        endfunction()
        aeg_set_mingw_runtime_for_registered_tests(aeg_application_platform_win32_smoke aeg_editor_imgui_smoke
                aeg_editor_imgui_docking_smoke aeg_editor_imgui_viewport_smoke aeg_editor_layout_smoke
                aeg_editor_workspace_smoke aeg_editor_font_smoke aeg_editor_font_validation_smoke
                aeg_editor_preferences_smoke aeg_editor_imgui_renderer_smoke aeg_math_smoke aeg_result_smoke
                aeg_reflection_smoke aeg_serialization_smoke aeg_concurrency_smoke aeg_ecs_smoke aeg_pipeline_smoke
                aeg_pipeline_extension_smoke aeg_pipeline_conformance_smoke aeg_pipeline_runtime_smoke
                aeg_pipeline_hot_swap_smoke aeg_pipeline_phase_scheduler_smoke aeg_module_smoke aeg_world_scope_smoke
                aeg_world_serialization_smoke aeg_scene_asset_smoke aeg_world_frame_orchestrator_smoke
                aeg_world_multi_view_smoke aeg_engine_frame_orchestrator_smoke aeg_runtime_components_scene_smoke
                aeg_runtime_component_orchestrator_smoke aeg_renderer_smoke aeg_opaque_render_feature_smoke
                aeg_opaque_render_feature_multiframe_smoke aeg_opaque_render_feature_uv_depth_smoke
                aeg_editor_scene_runtime_smoke aeg_editor_external_assets_smoke aeg_editor_viewport_interaction_smoke
                aeg_editor_history_smoke aeg_editor_hierarchy_smoke aeg_editor_inspector_smoke
                aeg_editor_asset_database_smoke aeg_editor_project_smoke aeg_render_feature_runtime_smoke
                aeg_frame_retention_smoke aeg_rhi_smoke aeg_logging_smoke aeg_triangle
                aeg_editor_application_identity_smoke aeg_editor_application aeg_editor_application_preferences
                aeg_editor_application_native_viewports
                aeg_editor_application_missing_project aeg_editor_error_smoke aeg_editor_asset_pipeline_recovery_smoke
                aeg_render_graph_imported_texture_state_smoke)
    endif()
endif()
