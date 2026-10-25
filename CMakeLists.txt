cmake_minimum_required(VERSION 3.25)
project(AEG_Engine LANGUAGES CXX)
enable_language(C)
if (MSVC)
    add_compile_options(/FS)
    if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "(^|[ \t])/FS($|[ \t])")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /FS" CACHE STRING "" FORCE)
    endif()
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
    Engine/Foundation/public/asset_identity.hpp
    Engine/Foundation/public/parameter_value.hpp
    Engine/Foundation/public/stable_id.hpp
    Engine/Foundation/private/result.cpp
    Engine/Foundation/private/asset_identity.cpp
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

add_library(aeg_application_authoring STATIC
    Application/Authoring/public/feature_graph_io.hpp
    Application/Authoring/private/feature_graph_io.cpp
    Application/Authoring/private/feature_graph_source_storage.cpp
)
target_compile_features(aeg_application_authoring PUBLIC cxx_std_20)
target_include_directories(aeg_application_authoring
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/toml11/include
)
target_link_libraries(aeg_application_authoring PUBLIC aeg_foundation aeg_feature)

if (MSVC)
    target_compile_options(aeg_application_authoring PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_application_authoring PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_application_project STATIC
    Application/Project/private/project_context.cpp
    Application/Project/private/project_scaffold.cpp
)
target_compile_features(aeg_application_project PUBLIC cxx_std_20)
target_include_directories(aeg_application_project
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/toml11/include
)
target_link_libraries(aeg_application_project PUBLIC aeg_foundation)
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
target_link_libraries(aeg_project_scaffold_smoke PRIVATE aeg_application_project aeg_application_authoring aeg_scene)
if (MSVC)
    target_compile_options(aeg_project_scaffold_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_project_scaffold_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
endif()
endif()

if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
add_library(aeg_editor_feature_model STATIC
    Application/Editor/private/feature_process_editor.hpp
    Application/Editor/private/feature_process_editor.cpp
    Application/Editor/private/composition/editor_feature_graph_model.cpp
    Application/Editor/private/composition/editor_feature_graph_preview.hpp
    Application/Editor/private/composition/editor_feature_graph_preview.cpp
    Application/Editor/private/composition/editor_feature_catalog_diff.cpp
    Application/Editor/private/composition/editor_feature_graph_document.cpp
    Application/Editor/private/composition/editor_feature_graph_panel.cpp
    Application/Editor/private/composition/editor_feature_graph_canvas.hpp
    Application/Editor/private/composition/editor_feature_graph_canvas.cpp
    Application/Editor/private/composition/editor_feature_graph_layout.hpp
    Application/Editor/private/composition/editor_feature_graph_layout.cpp
    Application/Editor/private/composition/editor_process_graph_canvas.cpp
    Application/Editor/private/composition/editor_render_graph_inspection.cpp
    Application/Editor/private/composition/editor_feature_definition_panel.cpp
    Application/Editor/private/composition/editor_feature_implementation_resolver.cpp
    Application/Editor/private/composition/editor_parameter_widget.hpp
    Application/Editor/private/composition/editor_custom_node_workflow.cpp
    Application/Editor/private/composition/editor_node_definition.cpp
    Application/Editor/private/composition/editor_node_contract_storage.cpp
    Application/Editor/private/composition/editor_node_definition_service.cpp
)
target_compile_features(aeg_editor_feature_model PUBLIC cxx_std_20)
target_include_directories(aeg_editor_feature_model
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Application/EditorFramework/public
           ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
           ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
           ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public)
target_include_directories(aeg_editor_feature_model PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
target_link_libraries(aeg_editor_feature_model PUBLIC aeg_application_project aeg_application_authoring aeg_feature aeg_feature_runtime_host)
target_link_libraries(aeg_editor_feature_model PRIVATE aeg_render_graph)
if (MSVC)
    target_compile_options(aeg_editor_feature_model PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_feature_model PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_editor_feature_graph_smoke tests/editor_feature_graph_smoke.cpp)
add_executable(aeg_editor_feature_graph_preview_smoke tests/editor_feature_graph_preview_smoke.cpp)
target_link_libraries(aeg_editor_feature_graph_preview_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_graph_preview_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_feature_palette_clock_smoke tests/editor_feature_palette_clock_smoke.cpp)
target_link_libraries(aeg_editor_feature_palette_clock_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_palette_clock_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_feature_palette_clock_ui_smoke tests/editor_feature_palette_clock_ui_smoke.cpp)
target_link_libraries(aeg_editor_feature_palette_clock_ui_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_palette_clock_ui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_editor_feature_edge_constraints_ui_smoke tests/editor_feature_edge_constraints_ui_smoke.cpp)

target_link_libraries(aeg_editor_feature_edge_constraints_ui_smoke PRIVATE aeg_editor_feature_model aeg_feature)

target_include_directories(aeg_editor_feature_edge_constraints_ui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_editor_feature_graph_native_scroll_smoke tests/editor_feature_graph_native_scroll_smoke.cpp)
target_link_libraries(aeg_editor_feature_graph_native_scroll_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_graph_native_scroll_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_editor_feature_execution_ui_smoke tests/editor_feature_execution_ui_smoke.cpp)
target_link_libraries(aeg_editor_feature_execution_ui_smoke PRIVATE
    aeg_editor_feature_model aeg_feature_runtime_host aeg_feature aeg_renderer aeg_render_graph)
target_include_directories(aeg_editor_feature_execution_ui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_feature_execution_inspection_smoke tests/feature_execution_inspection_smoke.cpp)
target_link_libraries(aeg_feature_execution_inspection_smoke PRIVATE
    aeg_editor_feature_model aeg_feature_runtime_host aeg_application_runtime aeg_renderer
    aeg_engine aeg_scene aeg_editor_framework)
target_include_directories(aeg_feature_execution_inspection_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
add_executable(aeg_editor_process_canvas_smoke tests/editor_process_canvas_smoke.cpp)
add_executable(aeg_editor_feature_graph_canvas_ui_smoke tests/editor_feature_graph_canvas_ui_smoke.cpp)
add_executable(aeg_editor_feature_graph_orchestration_ui_smoke tests/editor_feature_graph_orchestration_ui_smoke.cpp)
target_link_libraries(aeg_editor_feature_graph_orchestration_ui_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_graph_orchestration_ui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_editor_feature_graph_transactions_smoke tests/editor_feature_graph_transactions_smoke.cpp)
target_link_libraries(aeg_editor_feature_graph_transactions_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_graph_transactions_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_feature_implementation_resolver_smoke tests/editor_feature_implementation_resolver_smoke.cpp)
add_executable(aeg_editor_feature_definition_navigation_ui_smoke tests/editor_feature_definition_navigation_ui_smoke.cpp)
foreach(flow_navigation_test IN ITEMS aeg_editor_feature_implementation_resolver_smoke aeg_editor_feature_definition_navigation_ui_smoke)
    target_link_libraries(${flow_navigation_test} PRIVATE aeg_editor_feature_model)
    target_include_directories(${flow_navigation_test} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
endforeach()
target_link_libraries(aeg_editor_feature_graph_canvas_ui_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_graph_canvas_ui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
target_link_libraries(aeg_editor_process_canvas_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_process_canvas_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_editor_process_layout_smoke tests/editor_process_layout_smoke.cpp)
target_link_libraries(aeg_editor_process_layout_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_process_layout_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
target_link_libraries(aeg_editor_feature_graph_smoke PRIVATE aeg_editor_feature_model aeg_feature)
target_include_directories(aeg_editor_feature_graph_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
add_executable(aeg_editor_process_layout_boundary_smoke
    tests/editor_process_layout_boundary_smoke.cpp)
target_link_libraries(aeg_editor_process_layout_boundary_smoke PRIVATE aeg_editor_feature_model)
add_executable(aeg_editor_composition_boundary_smoke
    tests/editor_composition_boundary_smoke.cpp)
target_link_libraries(aeg_editor_composition_boundary_smoke PRIVATE aeg_editor_feature_model)
add_executable(aeg_editor_custom_node_workflow_smoke tests/editor_custom_node_workflow_smoke.cpp)
target_link_libraries(aeg_editor_custom_node_workflow_smoke PRIVATE aeg_editor_feature_model)
target_include_directories(aeg_editor_custom_node_workflow_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_feature_definition_ui_smoke tests/editor_feature_definition_ui_smoke.cpp)
target_link_libraries(aeg_editor_feature_definition_ui_smoke PRIVATE aeg_editor_feature_model)
target_include_directories(aeg_editor_feature_definition_ui_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_generated_project_module_smoke tests/editor_generated_project_module_smoke.cpp)
target_link_libraries(aeg_editor_generated_project_module_smoke PRIVATE aeg_editor_feature_model aeg_module)
target_include_directories(aeg_editor_generated_project_module_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_node_definition_model_smoke tests/editor_node_definition_model_smoke.cpp)
target_link_libraries(aeg_editor_node_definition_model_smoke PRIVATE aeg_editor_feature_model)
target_include_directories(aeg_editor_node_definition_model_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_node_contract_storage_smoke
    tests/editor_node_contract_storage_smoke.cpp
    Application/Editor/private/editor_project_feature_authoring.cpp)
target_link_libraries(aeg_editor_node_contract_storage_smoke PRIVATE aeg_editor_feature_model aeg_renderer)
target_include_directories(aeg_editor_node_contract_storage_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_node_definition_generation_smoke tests/editor_node_definition_generation_smoke.cpp)
target_link_libraries(aeg_editor_node_definition_generation_smoke PRIVATE aeg_editor_feature_model)
target_include_directories(aeg_editor_node_definition_generation_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_node_definition_service_smoke tests/editor_node_definition_service_smoke.cpp)
target_link_libraries(aeg_editor_node_definition_service_smoke PRIVATE aeg_editor_feature_model)
target_include_directories(aeg_editor_node_definition_service_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
add_executable(aeg_editor_node_definition_end_to_end_smoke
    tests/editor_node_definition_end_to_end_smoke.cpp
    Application/Editor/private/editor_project_feature_authoring.cpp)
target_link_libraries(aeg_editor_node_definition_end_to_end_smoke PRIVATE aeg_editor_feature_model aeg_renderer)
target_include_directories(aeg_editor_node_definition_end_to_end_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
if (MSVC)
    target_compile_options(aeg_editor_custom_node_workflow_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_node_definition_model_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_node_contract_storage_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_node_definition_generation_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_node_definition_service_smoke PRIVATE /W4 /permissive-)
    target_compile_options(aeg_editor_node_definition_end_to_end_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_custom_node_workflow_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_node_definition_model_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_node_contract_storage_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_node_definition_generation_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_node_definition_service_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aeg_editor_node_definition_end_to_end_smoke PRIVATE -Wall -Wextra -Wpedantic)
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
    Application/Tooling/private/feature_graph_asset_importer.cpp
    Application/Tooling/private/cooked_asset.cpp
    Application/Tooling/private/cooked_asset_encoder.cpp
    Application/Tooling/private/cooked_artifact_store.cpp
    Application/Tooling/private/project_operation.cpp
    Application/Tooling/private/project_module_agreement.cpp
    Application/Tooling/private/project_operation_feature.cpp
    Application/Tooling/private/project_process.cpp
    Application/Tooling/private/project_cooker.cpp
)
target_compile_features(aeg_application_tooling PUBLIC cxx_std_20)
target_include_directories(aeg_application_tooling
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Scene/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Serialization/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/toml11/include
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/stb
        ${AEG_COOKED_ASSET_GENERATED_DIR}
)
target_link_libraries(aeg_application_tooling PUBLIC aeg_foundation aeg_feature aeg_asset aeg_module aeg_application_project aeg_application_authoring flatbuffers PRIVATE aeg_concurrency aeg_feature_world_protocol)
add_dependencies(aeg_application_tooling aeg_cooked_asset_schema)

if (AEG_BUILD_TESTS)
    add_executable(aeg_feature_graph_editor_layout_cook_smoke tests/feature_graph_editor_layout_cook_smoke.cpp)
    target_link_libraries(aeg_feature_graph_editor_layout_cook_smoke PRIVATE aeg_application_tooling aeg_asset)
endif()

# Feature definition generation delegates project operations to the shared tooling service.
if (TARGET aeg_editor_feature_model)
    target_link_libraries(aeg_editor_feature_model PUBLIC aeg_application_tooling)
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
target_link_libraries(aeg_project_operation_smoke PRIVATE aeg_application_tooling aeg_concurrency)
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
        Application/Platform/private/application/application_platform_win32.cpp
        Application/Platform/private/application/win32_console_interrupt.cpp
        Application/Platform/private/application/win32_system_fonts.cpp
        Application/Platform/private/application/win32_startup_splash.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
    PRIVATE
        ${AEG_IMGUI_DIR}
)
target_link_libraries(aeg_editor_framework PUBLIC aeg_foundation aeg_platform aeg_input)
target_link_libraries(aeg_editor_framework PUBLIC aeg_imgui_core aeg_imgui_freetype)

if (AEG_BUILD_EDITOR AND TARGET aeg_editor_feature_model)
    target_include_directories(aeg_editor_feature_model PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/ImGui)
    target_link_libraries(aeg_editor_feature_model PUBLIC aeg_imgui_core)
endif()

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
target_link_libraries(aeg_editor_workspace_smoke PRIVATE aeg_editor_framework aeg_editor_feature_model)
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

# Shared API between engine implementation targets only. It is deliberately
# absent from the compile interface of AEG::GameSDK and AEG::GameRuntime.
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
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
)
target_link_libraries(aeg_ecs PUBLIC aeg_foundation aeg_reflection PRIVATE aeg_concurrency)

if (MSVC)
    target_compile_options(aeg_ecs PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_ecs PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_ecs_smoke
    tests/ecs_smoke.cpp
)
target_link_libraries(aeg_ecs_smoke PRIVATE aeg_ecs aeg_concurrency)
if (WIN32)
    target_compile_definitions(aeg_ecs_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_feature_world_protocol STATIC
    Engine/Feature/public/feature_world_transform.hpp
    Engine/Feature/private/feature_world_transform.cpp
    Engine/Runtime/private/world_transform_schema.cpp)
target_compile_features(aeg_feature_world_protocol PUBLIC cxx_std_20)
target_include_directories(aeg_feature_world_protocol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/public)
target_link_libraries(aeg_feature_world_protocol PUBLIC aeg_foundation
    PRIVATE aeg_serialization aeg_reflection aeg_math)

add_library(aeg_feature STATIC
    Engine/Feature/private/feature_execution.cpp
    Engine/Feature/private/feature_render_intent.cpp
    Engine/Feature/public/feature_types.hpp
    Engine/Feature/public/feature_time.hpp
    Engine/Feature/private/feature_time_source.hpp
    Engine/Feature/public/feature_contract.hpp
    Engine/Feature/public/feature_parameters.hpp
    Engine/Feature/public/feature_registry.hpp
    Engine/Feature/public/feature_runtime.hpp
    Engine/Feature/public/feature_graph.hpp
    Engine/Feature/public/feature_catalog.hpp
    Engine/Feature/public/feature_contract_json.hpp
    Engine/Feature/private/feature_contract_json.cpp
    Engine/Feature/public/feature_port_router.hpp
    Engine/Feature/public/feature_resource_bus.hpp
    Engine/Feature/public/feature_contract_metadata.hpp
    Engine/Feature/public/feature_process.hpp
    Engine/Feature/private/feature_contract.cpp
    Engine/Feature/private/feature_parameters.cpp
    Engine/Feature/private/feature_registry.cpp
    Engine/Feature/private/feature_clock_admission.hpp
    Engine/Feature/public/feature_clock_host.hpp
    Engine/Feature/private/feature_clock_admission.cpp
    Engine/Feature/private/feature_runtime.cpp
    Engine/Feature/private/feature_input.cpp
    Engine/Feature/private/feature_outbox.hpp
    Engine/Feature/public/render_intent_plan.hpp
    Engine/Feature/private/render_intent_planner.hpp
    Engine/Feature/private/render_intent_plan.cpp
    Engine/Feature/private/render_intent_evidence.hpp
    Engine/Feature/private/feature_graph.cpp
    Engine/Feature/private/feature_catalog.cpp
    Engine/Feature/private/feature_port_router.cpp
    Engine/Feature/private/feature_resource_bus.cpp
    Engine/Feature/private/feature_contract_metadata.cpp
    Engine/Feature/private/feature_process.cpp
)
target_compile_features(aeg_feature PUBLIC cxx_std_20)
target_include_directories(aeg_feature
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
)
target_link_libraries(aeg_feature PUBLIC aeg_foundation PRIVATE aeg_serialization aeg_concurrency)
if (MSVC)
    target_compile_options(aeg_feature PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_feature PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_pipeline STATIC
    Engine/Pipeline/private/render_pass_builder_backend.hpp
    Engine/Pipeline/private/render_pass_execution_context_backend.hpp
    Engine/Pipeline/private/render_graph_builder_backend.hpp
    Engine/Pipeline/private/render_pass_registry_execution.hpp
    Engine/Pipeline/private/phase_registry_execution.hpp
    Engine/Pipeline/private/pipeline_compiler_execution.hpp
    Engine/Pipeline/private/pipeline_scope_host_execution.hpp
    Engine/Pipeline/private/pipeline.cpp
    Engine/Pipeline/private/render_pass.cpp
    Engine/Pipeline/private/render_pass_execution.hpp
)
target_compile_features(aeg_pipeline PUBLIC cxx_std_20)
target_include_directories(aeg_pipeline
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
)
target_link_libraries(aeg_pipeline PRIVATE aeg_foundation aeg_feature)

if (MSVC)
    target_compile_options(aeg_pipeline PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_asset STATIC
    Engine/Asset/public/feature_graph_asset.hpp
    Engine/Asset/public/world_command_routes.hpp
    Engine/Asset/private/cooked_asset.cpp
    Engine/Asset/private/asset_package_reader.cpp
    Engine/Asset/private/asset_handle_table.cpp
    Engine/Asset/private/game_build_manifest.cpp
    Engine/Asset/private/world_command_routes.cpp
    Engine/Asset/private/mesh_payload.cpp
    Engine/Asset/private/texture_payload.cpp
    Engine/Asset/private/material_payload.cpp
    Engine/Asset/private/font_payload.cpp
    Engine/Asset/private/scene_payload.cpp
    Engine/Asset/private/feature_graph_asset.cpp
)
target_compile_features(aeg_asset PUBLIC cxx_std_20)
target_include_directories(aeg_asset
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
    PRIVATE ${AEG_COOKED_ASSET_GENERATED_DIR}
)
add_dependencies(aeg_asset aeg_cooked_asset_schema)
target_link_libraries(aeg_asset PUBLIC aeg_foundation aeg_feature aeg_serialization flatbuffers)
if (MSVC)
    target_compile_options(aeg_asset PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_asset PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_feature_runtime_host STATIC
    Application/Runtime/private/runtime_feature_control.hpp
    Application/Runtime/private/feature_graph_runtime_host.hpp
    Application/Runtime/private/feature_graph_runtime_host.cpp
)
target_compile_features(aeg_feature_runtime_host PUBLIC cxx_std_20)
target_include_directories(aeg_feature_runtime_host
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
)
target_link_libraries(aeg_feature_runtime_host PUBLIC aeg_feature aeg_asset)
if (MSVC)
    target_compile_options(aeg_feature_runtime_host PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_feature_runtime_host PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (AEG_BUILD_TESTS)
add_executable(aeg_pipeline_smoke
    tests/pipeline_smoke.cpp
)
target_link_libraries(aeg_pipeline_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_contract_smoke
    tests/feature_contract_smoke.cpp
)
target_link_libraries(aeg_feature_contract_smoke PRIVATE aeg_feature)
add_executable(aeg_feature_clock_contract_smoke tests/feature_clock_contract_smoke.cpp)
target_link_libraries(aeg_feature_clock_contract_smoke PRIVATE aeg_feature)
target_include_directories(aeg_feature_clock_contract_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include)
add_executable(aeg_feature_clock_deadline_smoke tests/feature_clock_deadline_smoke.cpp)
target_link_libraries(aeg_feature_clock_deadline_smoke PRIVATE aeg_feature aeg_concurrency)
target_include_directories(aeg_feature_clock_deadline_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include)
add_executable(aeg_feature_clock_admission_smoke tests/feature_clock_admission_smoke.cpp)
target_link_libraries(aeg_feature_clock_admission_smoke PRIVATE aeg_feature aeg_module)
if (WIN32)
    target_link_libraries(aeg_feature_clock_admission_smoke PRIVATE dbghelp)
endif()
target_include_directories(aeg_feature_clock_admission_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private)
add_executable(aeg_feature_clock_runtime_admission_smoke tests/feature_clock_runtime_admission_smoke.cpp)
target_link_libraries(aeg_feature_clock_runtime_admission_smoke PRIVATE aeg_feature_runtime_host)
target_include_directories(aeg_feature_clock_runtime_admission_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
if (WIN32)
    target_compile_definitions(aeg_feature_contract_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_outbox_smoke tests/feature_outbox_smoke.cpp)
target_link_libraries(aeg_feature_outbox_smoke PRIVATE aeg_feature aeg_concurrency)

add_executable(aeg_feature_process_transition_order_smoke tests/feature_process_transition_order_smoke.cpp)
target_link_libraries(aeg_feature_process_transition_order_smoke PRIVATE aeg_feature aeg_concurrency)

add_executable(aeg_feature_runtime_smoke
    tests/feature_runtime_smoke.cpp
)
target_link_libraries(aeg_feature_runtime_smoke PRIVATE aeg_feature)
if (WIN32)
    target_compile_definitions(aeg_feature_runtime_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_asset_parameters_smoke tests/feature_asset_parameters_smoke.cpp)
target_link_libraries(aeg_feature_asset_parameters_smoke PRIVATE aeg_feature aeg_application_tooling)

add_executable(aeg_feature_runtime_regression_smoke
    tests/feature_runtime_regression_smoke.cpp
)
target_link_libraries(aeg_feature_runtime_regression_smoke PRIVATE aeg_feature)
if (WIN32)
    target_compile_definitions(aeg_feature_runtime_regression_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_async_execution_smoke tests/feature_async_execution_smoke.cpp)
target_link_libraries(aeg_feature_async_execution_smoke PRIVATE aeg_feature aeg_concurrency)
target_include_directories(aeg_feature_async_execution_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private)

add_executable(aeg_feature_transport_smoke
    tests/feature_transport_smoke.cpp
)
target_link_libraries(aeg_feature_transport_smoke PRIVATE aeg_feature)
if (WIN32)
    target_compile_definitions(aeg_feature_transport_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_transport_semantics_smoke
    tests/feature_transport_semantics_smoke.cpp
)
target_link_libraries(aeg_feature_transport_semantics_smoke PRIVATE aeg_feature)
if (WIN32)
    target_compile_definitions(aeg_feature_transport_semantics_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_graph_authoring_smoke
    tests/feature_graph_authoring_smoke.cpp
)
target_link_libraries(aeg_feature_graph_authoring_smoke PRIVATE aeg_feature)
if (WIN32)
    target_compile_definitions(aeg_feature_graph_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_graph_runtime_e2e_smoke
    tests/feature_graph_runtime_e2e_smoke.cpp
)
target_link_libraries(aeg_feature_graph_runtime_e2e_smoke PRIVATE aeg_feature_runtime_host)
target_include_directories(aeg_feature_graph_runtime_e2e_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/abi
)
if (WIN32)
    target_compile_definitions(aeg_feature_graph_runtime_e2e_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_graph_document_io_smoke
    tests/feature_graph_document_io_smoke.cpp
)
target_link_libraries(aeg_feature_graph_document_io_smoke PRIVATE aeg_application_authoring aeg_concurrency)
target_include_directories(aeg_feature_graph_document_io_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/private)
add_executable(aeg_feature_authoring_boundary_smoke tests/feature_authoring_boundary_smoke.cpp)
target_link_libraries(aeg_feature_authoring_boundary_smoke PRIVATE aeg_application_authoring)
if (WIN32)
    target_compile_definitions(aeg_feature_graph_document_io_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_render_projection_smoke
    tests/feature_render_projection_smoke.cpp
)
target_link_libraries(aeg_feature_render_projection_smoke PRIVATE aeg_render_graph)
target_include_directories(aeg_feature_render_projection_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private)
add_executable(aeg_feature_render_execution_smoke tests/feature_render_execution_smoke.cpp)
target_link_libraries(aeg_feature_render_execution_smoke PRIVATE aeg_render_graph aeg_rhi)
target_include_directories(aeg_feature_render_execution_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
add_executable(aeg_feature_render_history_smoke tests/feature_render_history_smoke.cpp)
target_link_libraries(aeg_feature_render_history_smoke PRIVATE aeg_render_graph aeg_rhi)
target_include_directories(aeg_feature_render_history_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
add_executable(aeg_render_feature_history_boundary_smoke
    tests/render_feature_history_boundary_smoke.cpp)
target_link_libraries(aeg_render_feature_history_boundary_smoke PRIVATE aeg_render_graph)
if (WIN32 AND TARGET aeg_application_platform)
    target_link_libraries(aeg_feature_render_execution_smoke PRIVATE aeg_application_platform)
    target_compile_definitions(aeg_feature_render_execution_smoke PRIVATE AEG_TEST_FEATURE_PRESENT)
endif()
if (WIN32)
    target_compile_definitions(aeg_feature_render_projection_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_no_legacy_identity_smoke
    tests/feature_no_legacy_identity_smoke.cpp
)
if (WIN32)
    target_compile_definitions(aeg_feature_no_legacy_identity_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_module_authoring_smoke
    tests/module_authoring_smoke.cpp
)
target_link_libraries(aeg_module_authoring_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_module_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_runtime_asset_package_smoke
    tests/runtime_asset_package_smoke.cpp
)
target_include_directories(aeg_runtime_asset_package_smoke PRIVATE ${AEG_COOKED_ASSET_GENERATED_DIR})
target_link_libraries(aeg_runtime_asset_package_smoke PRIVATE aeg_asset flatbuffers)
add_executable(aeg_game_build_manifest_smoke tests/game_build_manifest_smoke.cpp)
target_link_libraries(aeg_game_build_manifest_smoke PRIVATE aeg_asset)
add_executable(aeg_world_command_routes_smoke tests/world_command_routes_smoke.cpp)
target_link_libraries(aeg_world_command_routes_smoke PRIVATE aeg_asset)
add_executable(aeg_cooked_scene_dependencies_smoke
    tests/cooked_scene_dependencies_smoke.cpp
)
target_link_libraries(aeg_cooked_scene_dependencies_smoke PRIVATE aeg_asset)

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
    Engine/Module/private/feature_callback_codec.hpp
    Engine/Module/private/feature_callback_codec.cpp
    Engine/Module/private/feature_module_export.cpp
    Engine/Module/public/feature_module_export.hpp
)
if (AEG_BUILD_TESTS)
    add_executable(aeg_module_abi_header tests/module_abi_header.c)
    add_executable(aeg_module_abi_smoke tests/module_abi_smoke.cpp)
    target_link_libraries(aeg_module_abi_smoke PRIVATE aeg_module)
    add_executable(aeg_module_feature_callback_codec_smoke tests/module_feature_callback_codec_smoke.cpp)
    target_link_libraries(aeg_module_feature_callback_codec_smoke PRIVATE aeg_module)
    add_executable(aeg_module_metadata_smoke tests/module_metadata_smoke.cpp)
    target_link_libraries(aeg_module_metadata_smoke PRIVATE aeg_module)
    add_executable(aeg_module_dynamic_lifetime_smoke tests/module_dynamic_lifetime_smoke.cpp)
    target_link_libraries(aeg_module_dynamic_lifetime_smoke PRIVATE aeg_module aeg_render_graph aeg_rhi aeg_concurrency)
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
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/abi
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
)
target_link_libraries(aeg_module PUBLIC aeg_foundation aeg_reflection aeg_feature PRIVATE aeg_serialization)

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

if (AEG_BUILD_TESTS)
add_executable(aeg_module_feature_registration_smoke
    tests/module_feature_registration_smoke.cpp
)
target_link_libraries(aeg_module_feature_registration_smoke PRIVATE aeg_module)
if (WIN32)
    target_compile_definitions(aeg_module_feature_registration_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif()

add_library(aeg_world STATIC
    Engine/World/private/world_scope.cpp
    Engine/World/private/world_snapshot_execution.hpp
    Engine/World/private/world_generation_access.hpp
    Engine/World/private/world_serializer.cpp
)
target_compile_features(aeg_world PUBLIC cxx_std_20)
target_include_directories(aeg_world
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/World/public
)
target_link_libraries(aeg_world PUBLIC aeg_ecs aeg_serialization PRIVATE aeg_pipeline)

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
add_executable(aeg_world_public_boundary_smoke tests/world_public_boundary_smoke.cpp)
target_link_libraries(aeg_world_public_boundary_smoke PRIVATE aeg_world)
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

add_executable(aeg_feature_module_authoring_smoke
    tests/feature_module_authoring_smoke.cpp
)
target_link_libraries(aeg_feature_module_authoring_smoke PRIVATE aeg_module)
if (WIN32)
    target_compile_definitions(aeg_feature_module_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_feature_module_header_only_smoke
    tests/feature_module_header_only_smoke.cpp
)
target_include_directories(aeg_feature_module_header_only_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Foundation/public)
if (WIN32)
    target_compile_definitions(aeg_feature_module_header_only_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
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
target_link_libraries(aeg_world_frame_orchestrator_smoke PRIVATE aeg_engine aeg_world aeg_rhi aeg_feature)
add_executable(aeg_world_feature_commands_smoke tests/world_feature_commands_smoke.cpp)
target_link_libraries(aeg_world_feature_commands_smoke PRIVATE aeg_engine aeg_feature aeg_concurrency)
add_executable(aeg_world_raw_pipeline_rejection_smoke tests/world_raw_pipeline_rejection_smoke.cpp)
target_link_libraries(aeg_world_raw_pipeline_rejection_smoke PRIVATE aeg_engine aeg_rhi)
target_include_directories(aeg_world_raw_pipeline_rejection_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
target_include_directories(aeg_world_frame_orchestrator_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/World/private)
if (WIN32)
    target_compile_definitions(aeg_world_frame_orchestrator_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_world_multi_view_smoke
    tests/world_multi_view_smoke.cpp
)
target_link_libraries(aeg_world_multi_view_smoke PRIVATE aeg_engine aeg_world aeg_rhi)
target_include_directories(aeg_world_multi_view_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/World/private)
if (WIN32)
    target_compile_definitions(aeg_world_multi_view_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
endif() # AEG_BUILD_TESTS

add_library(aeg_renderer STATIC
    Engine/Renderer/private/render_feature_context.hpp
    Engine/Renderer/private/native_scene_pass_source.hpp
    Engine/Renderer/private/light_culling_origin.hpp
    Engine/Renderer/private/clustered_forward.cpp
    Engine/Renderer/private/render_feature_draw.cpp
    Engine/Renderer/private/post_process.cpp
    Engine/Renderer/private/light_culling.cpp
    Engine/Renderer/private/shadow_rendering.cpp
    Engine/Renderer/private/shadow_resources.cpp
    Engine/Renderer/private/deferred_lighting_resources.cpp
    Engine/Renderer/private/builtin_render_passes.cpp
    Engine/Renderer/private/render_pipeline_graph.cpp
    Engine/Renderer/private/renderer_feature_profiles.cpp
    Engine/Renderer/private/opaque_render_feature.cpp
    Engine/Renderer/private/renderer.cpp
    Engine/Renderer/private/render_pipeline_variants.cpp
)
target_compile_features(aeg_renderer PUBLIC cxx_std_20)
target_include_directories(aeg_renderer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
)
target_link_libraries(aeg_renderer
    PUBLIC
        aeg_ecs
        aeg_math
        aeg_feature
        aeg_rhi_core
    PRIVATE
        aeg_pipeline
        aeg_world
        aeg_shader
        aeg_render_graph
        aeg_feature
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
    Engine/Shader/private/shader_pipeline_layout_access.hpp
    Engine/Shader/private/shader_manager.cpp
)
target_compile_features(aeg_shader PUBLIC cxx_std_20)
target_include_directories(aeg_shader
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shader/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
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
    Engine/RenderGraph/private/render_feature_projection.cpp
    Engine/RenderGraph/private/render_feature_history.cpp
    Engine/RenderGraph/private/render_feature_history.hpp
    Engine/RenderGraph/private/render_feature_history_access.hpp
    Engine/RenderGraph/private/render_graph_submission.hpp
    Engine/RenderGraph/private/render_graph_upload_submission.hpp
    Engine/RenderGraph/private/render_graph_feature_access.hpp
    Engine/RenderGraph/private/render_graph_upload_access.hpp
    Engine/RenderGraph/private/render_graph_execution.hpp
    Engine/RenderGraph/private/render_graph_resource_access.hpp
    Engine/RenderGraph/private/render_scene_lowerer.hpp
    Engine/RenderGraph/private/render_feature_projector.hpp
    Engine/RenderGraph/public/render_feature_projection.hpp
)
target_compile_features(aeg_render_graph PUBLIC cxx_std_20)
target_include_directories(aeg_render_graph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
)
target_link_libraries(aeg_render_graph PUBLIC aeg_foundation aeg_rhi_core aeg_feature PRIVATE aeg_pipeline)

if (MSVC)
    target_compile_options(aeg_render_graph PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_render_graph PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(aeg_engine STATIC
    Engine/Runtime/private/engine_frame_orchestrator.cpp
    Engine/Runtime/private/feature_world_commands.cpp
    Engine/Runtime/private/feature_world_commands.hpp
    Engine/Runtime/private/engine_frame_inspection.hpp
    Engine/Runtime/private/engine_frame_world_commands.hpp
    Engine/Runtime/private/engine_frame_pipeline_host.hpp
    Engine/Runtime/private/engine_frame_pipeline_registration.hpp
    Engine/Runtime/private/runtime_components.cpp
    Engine/Runtime/private/runtime_session.cpp
)
target_compile_features(aeg_engine PUBLIC cxx_std_20)
target_include_directories(aeg_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/World/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
)
target_link_libraries(aeg_engine PUBLIC aeg_foundation aeg_render_graph aeg_world aeg_renderer
    PRIVATE aeg_pipeline aeg_feature aeg_feature_world_protocol aeg_serialization)

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
    Application/Runtime/private/runtime_mesh_vertex.cpp
    Application/Runtime/private/runtime_bootstrap.cpp
    Application/Runtime/private/runtime_scene_loader.cpp
)
target_compile_features(aeg_application_runtime PUBLIC cxx_std_20)
target_include_directories(aeg_application_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Foundation/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Asset/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Input/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Resource/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shader/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/abi
)
target_link_libraries(aeg_application_runtime PRIVATE
    aeg_asset aeg_feature aeg_feature_runtime_host aeg_module aeg_scene aeg_engine aeg_renderer aeg_resource aeg_input aeg_shader aeg_rhi)
if (WIN32 AND TARGET aeg_application_platform)
    target_link_libraries(aeg_application_runtime PRIVATE aeg_application_platform)
endif()
if (MSVC)
    target_compile_options(aeg_application_runtime PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_application_runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_cooked_mesh_layout_smoke
    tests/cooked_mesh_layout_smoke.cpp
)
target_link_libraries(aeg_cooked_mesh_layout_smoke PRIVATE aeg_application_runtime)

if (AEG_BUILD_TESTS)
add_executable(aeg_engine_public_boundary_smoke tests/engine_public_boundary_smoke.cpp)
add_executable(aeg_frame_task_owner_boundary_smoke tests/frame_task_owner_boundary_smoke.cpp)
target_link_libraries(aeg_frame_task_owner_boundary_smoke PRIVATE aeg_engine aeg_rhi)
target_include_directories(aeg_frame_task_owner_boundary_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
add_executable(aeg_pipeline_registration_owner_boundary_smoke tests/pipeline_registration_owner_boundary_smoke.cpp)
target_link_libraries(aeg_pipeline_registration_owner_boundary_smoke PRIVATE aeg_engine aeg_rhi)
target_include_directories(aeg_pipeline_registration_owner_boundary_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
add_executable(aeg_pipeline_value_validation_smoke tests/pipeline_value_validation_smoke.cpp)
target_link_libraries(aeg_pipeline_value_validation_smoke PRIVATE aeg_pipeline)
add_executable(aeg_render_graph_pass_inspection_smoke tests/render_graph_pass_inspection_smoke.cpp)
target_link_libraries(aeg_render_graph_pass_inspection_smoke PRIVATE aeg_render_graph aeg_rhi)
target_include_directories(aeg_render_graph_pass_inspection_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
target_link_libraries(aeg_engine_public_boundary_smoke PRIVATE aeg_engine aeg_application_tooling)
if (AEG_BUILD_EDITOR)
    target_link_libraries(aeg_engine_public_boundary_smoke PRIVATE aeg_editor_framework)
    target_compile_definitions(aeg_engine_public_boundary_smoke PRIVATE AEG_PUBLIC_EDITOR_CONSUMER=1)
endif()
endif()

# Public consumer-facing SDK names. The aliases keep game CMake files stable
# while the implementation target names remain private to the engine build.
if (NOT TARGET AEG::GameSDK)
    add_library(aeg_game_sdk INTERFACE)
    target_compile_features(aeg_game_sdk INTERFACE cxx_std_20)
    # Game authors consume only the Feature/Module authoring facade.  The
    # implementation library is link-only so Pipeline, Runtime, and
    # Concurrency usage requirements never become game compile requirements.
    target_include_directories(aeg_game_sdk INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Foundation/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
    )
    target_link_libraries(aeg_game_sdk INTERFACE "$<LINK_ONLY:aeg_module>" "$<LINK_ONLY:aeg_feature_world_protocol>")
    add_library(AEG::GameSDK ALIAS aeg_game_sdk)
endif()
if (NOT TARGET AEG::GameRuntime)
    add_library(aeg_game_runtime INTERFACE)
    # The game host sees the narrow startup facade only.  Runtime and Engine
    # remain link-only implementation dependencies and do not propagate their
    # public Pipeline/Concurrency include directories to game sources.
    target_include_directories(aeg_game_runtime INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/public/game
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Input/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/public
    )
    target_link_libraries(aeg_game_runtime INTERFACE
        aeg_game_sdk
        "$<LINK_ONLY:aeg_application_runtime>"
        "$<LINK_ONLY:aeg_engine>"
    )
    add_library(AEG::GameRuntime ALIAS aeg_game_runtime)
endif()

if (AEG_BUILD_TESTS)
if (TARGET aeg_editor_custom_node_workflow_smoke)
    set(AEG_GENERATED_FEATURE_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_feature_sdk_fixture")
    set(AEG_GENERATED_FEATURE_FIXTURE_FILES "")
    foreach(name EditorCompositeNode EditorFeatureNode0 EditorFeatureNode1 EditorFeatureNode2 EditorFeatureNode3 EditorFeatureNode4)
        foreach(suffix hpp cpp registration.cpp contract.json)
            list(APPEND AEG_GENERATED_FEATURE_FIXTURE_FILES "${AEG_GENERATED_FEATURE_FIXTURE_DIR}/Source/FlowGraph/${name}.flowgraph.${suffix}")
        endforeach()
    endforeach()
    add_custom_command(OUTPUT ${AEG_GENERATED_FEATURE_FIXTURE_FILES}
        COMMAND $<TARGET_FILE:aeg_editor_custom_node_workflow_smoke> --generate "${AEG_GENERATED_FEATURE_FIXTURE_DIR}"
        DEPENDS aeg_editor_custom_node_workflow_smoke
        VERBATIM)
    add_executable(aeg_editor_generated_feature_sdk_smoke
        tests/editor_generated_feature_sdk_smoke.cpp ${AEG_GENERATED_FEATURE_FIXTURE_FILES})
    target_include_directories(aeg_editor_generated_feature_sdk_smoke PRIVATE
        "${AEG_GENERATED_FEATURE_FIXTURE_DIR}/Source/FlowGraph")
    target_link_libraries(aeg_editor_generated_feature_sdk_smoke PRIVATE AEG::GameSDK)
endif()
add_library(aeg_module_dynamic_lifetime_fixture SHARED tests/module_dynamic_lifetime_fixture.cpp)
target_link_libraries(aeg_module_dynamic_lifetime_fixture PRIVATE AEG::GameSDK)
add_library(aeg_module_dynamic_lifetime_fixture_v2 SHARED tests/module_dynamic_lifetime_fixture.cpp)
target_compile_definitions(aeg_module_dynamic_lifetime_fixture_v2 PRIVATE AEG_DYNAMIC_FIXTURE_REVISION=2)
target_link_libraries(aeg_module_dynamic_lifetime_fixture_v2 PRIVATE AEG::GameSDK)
add_library(aeg_module_world_motion_fixture SHARED tests/module_world_motion_fixture.cpp)
# Only the internal test gate uses Concurrency; the game-facing SDK is unchanged.
target_link_libraries(aeg_module_world_motion_fixture PRIVATE AEG::GameSDK aeg_concurrency)
add_library(aeg_demo_game_module SHARED DemoProject/Source/GameModule.cpp)
set_target_properties(aeg_demo_game_module PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(aeg_demo_game_module PRIVATE DemoProject/Source)
target_link_libraries(aeg_demo_game_module PRIVATE AEG::GameSDK)
add_dependencies(aeg_module_dynamic_lifetime_smoke aeg_module_dynamic_lifetime_fixture aeg_module_dynamic_lifetime_fixture_v2)
add_dependencies(aeg_project_operation_smoke aeg_module_dynamic_lifetime_fixture)
add_dependencies(aeg_project_cooker_smoke aeg_module_dynamic_lifetime_fixture)
if (TARGET aeg_editor_feature_model)
    add_executable(aeg_feature_end_to_end_smoke tests/feature_end_to_end_smoke.cpp)
    target_include_directories(aeg_feature_end_to_end_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition)
    target_link_libraries(aeg_feature_end_to_end_smoke PRIVATE
        aeg_editor_feature_model
        aeg_application_tooling
        aeg_application_runtime
        aeg_render_graph
        aeg_rhi
        aeg_concurrency)
    add_dependencies(aeg_feature_end_to_end_smoke aeg_module_dynamic_lifetime_fixture)
    if (WIN32)
        target_compile_definitions(aeg_feature_end_to_end_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
add_executable(aeg_game_sdk_boundary_smoke tests/game_sdk_boundary_smoke.cpp)
target_link_libraries(aeg_game_sdk_boundary_smoke PRIVATE AEG::GameRuntime)
add_executable(aeg_runtime_feature_inspect_boundary_smoke
    tests/runtime_feature_inspect_boundary_smoke.cpp)
target_link_libraries(aeg_runtime_feature_inspect_boundary_smoke PRIVATE aeg_feature_runtime_host)
add_executable(aeg_runtime_upload_host_boundary_smoke tests/runtime_upload_host_boundary_smoke.cpp)
target_link_libraries(aeg_runtime_upload_host_boundary_smoke PRIVATE aeg_application_runtime)
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
add_executable(aeg_runtime_world_commands_smoke tests/runtime_world_commands_smoke.cpp)
add_executable(aeg_world_transform_protocol_smoke tests/world_transform_protocol_smoke.cpp)
add_executable(aeg_feature_host_input_smoke tests/feature_host_input_smoke.cpp)
target_link_libraries(aeg_feature_host_input_smoke PRIVATE aeg_feature aeg_concurrency)
target_include_directories(aeg_feature_host_input_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private)
target_link_libraries(aeg_world_transform_protocol_smoke PRIVATE
    aeg_engine aeg_feature_world_protocol aeg_serialization)
target_include_directories(aeg_world_transform_protocol_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
target_link_libraries(aeg_runtime_world_commands_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling aeg_engine aeg_feature aeg_concurrency aeg_rhi)
target_include_directories(aeg_runtime_world_commands_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
target_include_directories(aeg_runtime_boot_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public)
    target_link_libraries(aeg_runtime_boot_smoke PRIVATE
        aeg_application_runtime aeg_application_tooling aeg_concurrency aeg_rhi
        aeg_engine aeg_scene aeg_renderer aeg_resource aeg_render_graph aeg_shader)
if (WIN32 AND AEG_BUILD_EDITOR)
    add_executable(aeg_runtime_owned_host_boot_smoke tests/runtime_owned_host_boot_smoke.cpp)
    target_include_directories(aeg_runtime_owned_host_boot_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Tooling/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
    target_link_libraries(aeg_runtime_owned_host_boot_smoke PRIVATE
        aeg_application_runtime aeg_application_tooling aeg_engine aeg_scene aeg_renderer
        aeg_resource aeg_render_graph aeg_shader aeg_rhi aeg_concurrency)
    target_compile_definitions(aeg_runtime_owned_host_boot_smoke PRIVATE
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}")
endif()
add_executable(aeg_runtime_scene_boot_smoke tests/runtime_scene_boot_smoke.cpp)
target_link_libraries(aeg_runtime_scene_boot_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling aeg_engine aeg_scene aeg_renderer
    aeg_resource aeg_render_graph aeg_shader)
add_executable(aeg_runtime_asset_resolution_smoke tests/runtime_asset_resolution_smoke.cpp)
target_link_libraries(aeg_runtime_asset_resolution_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling aeg_renderer aeg_resource aeg_render_graph aeg_shader aeg_rhi)
add_executable(aeg_runtime_font_cache_smoke tests/runtime_font_cache_smoke.cpp)
target_link_libraries(aeg_runtime_font_cache_smoke PRIVATE aeg_application_runtime aeg_application_tooling)
add_executable(aeg_shader_contract_smoke tests/shader_contract_smoke.cpp)
target_link_libraries(aeg_shader_contract_smoke PRIVATE aeg_shader)
add_executable(aeg_shader_cache_epoch_smoke tests/shader_cache_epoch_smoke.cpp)
target_link_libraries(aeg_shader_cache_epoch_smoke PRIVATE aeg_shader aeg_rhi)
add_executable(aeg_runtime_material_contract_smoke tests/runtime_material_contract_smoke.cpp)
target_link_libraries(aeg_runtime_material_contract_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling aeg_renderer aeg_resource aeg_render_graph aeg_shader aeg_rhi)
add_executable(aeg_runtime_deferred_contract_smoke tests/runtime_deferred_contract_smoke.cpp)
target_link_libraries(aeg_runtime_deferred_contract_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling aeg_renderer aeg_resource aeg_render_graph aeg_shader aeg_rhi)
add_executable(aeg_runtime_renderer_resource_contract_smoke
    tests/runtime_renderer_resource_contract_smoke.cpp)
target_link_libraries(aeg_runtime_renderer_resource_contract_smoke PRIVATE
    aeg_application_runtime aeg_application_tooling aeg_renderer aeg_resource aeg_render_graph aeg_shader aeg_rhi)

add_executable(aeg_runtime_object_vulkan_smoke tests/runtime_object_vulkan_smoke.cpp)
target_link_libraries(aeg_runtime_object_vulkan_smoke PRIVATE
    aeg_application_runtime aeg_renderer aeg_render_graph aeg_rhi aeg_engine aeg_resource aeg_shader)
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
target_include_directories(aeg_engine_frame_orchestrator_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
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
target_include_directories(aeg_runtime_component_orchestrator_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
if (WIN32)
    target_compile_definitions(aeg_runtime_component_orchestrator_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_smoke
    tests/renderer_smoke.cpp
)
target_link_libraries(aeg_renderer_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
target_include_directories(aeg_renderer_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
add_executable(aeg_renderer_feature_context_boundary_smoke
    tests/renderer_feature_context_boundary_smoke.cpp)
target_link_libraries(aeg_renderer_feature_context_boundary_smoke PRIVATE aeg_renderer)
add_executable(aeg_renderer_shader_contract_boundary_smoke
    tests/renderer_shader_contract_boundary_smoke.cpp)
target_link_libraries(aeg_renderer_shader_contract_boundary_smoke PRIVATE aeg_renderer)
add_executable(aeg_renderer_profile_contract_smoke
    tests/renderer_profile_contract_smoke.cpp)
target_link_libraries(aeg_renderer_profile_contract_smoke PRIVATE aeg_renderer)
if (WIN32)
    target_compile_definitions(aeg_renderer_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_pipeline_variants_smoke
    tests/renderer_pipeline_variants_smoke.cpp
)
target_link_libraries(aeg_renderer_pipeline_variants_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
target_include_directories(aeg_renderer_pipeline_variants_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private)
if (WIN32)
    target_compile_definitions(aeg_renderer_pipeline_variants_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_builtin_pass_smoke tests/renderer_builtin_pass_smoke.cpp)
add_executable(aeg_renderer_shadow_view_smoke tests/renderer_shadow_view_smoke.cpp)
target_link_libraries(aeg_renderer_shadow_view_smoke PRIVATE aeg_renderer aeg_render_graph)
target_include_directories(aeg_renderer_shadow_view_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public)
target_link_libraries(aeg_renderer_builtin_pass_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
target_include_directories(aeg_renderer_builtin_pass_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private)
if (WIN32)
    target_compile_definitions(aeg_renderer_builtin_pass_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_renderer_authoring_smoke tests/renderer_authoring_smoke.cpp)
add_executable(aeg_renderer_feature_profiles_smoke tests/renderer_feature_profiles_smoke.cpp)
target_link_libraries(aeg_renderer_feature_profiles_smoke PRIVATE aeg_renderer aeg_feature aeg_render_graph)
add_executable(aeg_editor_renderer_feature_host_smoke tests/editor_renderer_feature_host_smoke.cpp)
target_sources(aeg_editor_renderer_feature_host_smoke PRIVATE
    Application/Editor/private/editor_renderer_feature_host.cpp)
target_include_directories(aeg_editor_renderer_feature_host_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private)
target_link_libraries(aeg_editor_renderer_feature_host_smoke PRIVATE
    aeg_renderer aeg_feature_runtime_host aeg_feature)
add_executable(aeg_renderer_pipeline_graph_smoke tests/renderer_pipeline_graph_smoke.cpp)
target_link_libraries(aeg_renderer_pipeline_graph_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
target_include_directories(aeg_renderer_pipeline_graph_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
target_link_libraries(aeg_renderer_authoring_smoke PRIVATE aeg_renderer aeg_feature aeg_render_graph aeg_application_authoring)
target_include_directories(aeg_renderer_authoring_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private)
if (WIN32)
    target_compile_definitions(aeg_renderer_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_executable(aeg_frame_retention_smoke tests/frame_retention_smoke.cpp)
target_link_libraries(aeg_frame_retention_smoke PRIVATE aeg_render_graph aeg_rhi)
add_executable(aeg_render_graph_context_contract_smoke tests/render_graph_context_contract_smoke.cpp)
target_link_libraries(aeg_render_graph_context_contract_smoke PRIVATE aeg_render_graph)
add_executable(aeg_render_graph_imported_texture_state_smoke tests/render_graph_imported_texture_state_smoke.cpp)
target_link_libraries(aeg_render_graph_imported_texture_state_smoke PRIVATE aeg_render_graph aeg_rhi)
add_executable(aeg_render_graph_imported_buffer_smoke tests/render_graph_imported_buffer_smoke.cpp)
target_link_libraries(aeg_render_graph_imported_buffer_smoke PRIVATE aeg_render_graph aeg_rhi)
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
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
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
target_link_libraries(aeg_editor_assets PUBLIC aeg_foundation aeg_application_project aeg_concurrency)
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
    Application/EditorFramework/public/imgui_render_execution.hpp
)
target_include_directories(aeg_editor_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Math/public
)
target_link_libraries(aeg_editor_framework PUBLIC aeg_rhi PRIVATE aeg_resource aeg_shader aeg_pipeline aeg_render_graph)
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
    add_executable(aeg_feature_authoring_metadata_generator tools/feature_authoring_metadata_generator.cpp)
    target_link_libraries(aeg_feature_authoring_metadata_generator PRIVATE
        aeg_module aeg_feature aeg_feature_world_protocol aeg_application_authoring)
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
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/private
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
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
        TARGET aeg_core_shaders NAME forward_clustered_unshadowed
        OUTPUT_DIRECTORY "${AEG_SHADER_OUTPUT_DIR}/core_contract" CONTRACT_TOOL aeg_shader_contract_tool
        DEPENDENCIES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/shadow/visibility.hlsli"
        STAGES
            "vertex|vs_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/opaque/unlit_mesh.vert.hlsl"
            "fragment|ps_6_0|${CMAKE_CURRENT_SOURCE_DIR}/shaders/core/lighting/forward_clustered.frag.hlsl|main_unshadowed"
        PACKAGE_OUTPUT_VARIABLE AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE)
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
            AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
            AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
            AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}")
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
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
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
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}")
    add_dependencies(aeg_application_runtime aeg_core_shaders)
    if (TARGET aeg_runtime_owned_host_boot_smoke)
        add_dependencies(aeg_runtime_owned_host_boot_smoke aeg_core_shaders)
    endif()
    add_dependencies(aeg_editor_framework aeg_core_shaders)

if (AEG_BUILD_TESTS)
    add_executable(aeg_editor_imgui_renderer_smoke
        tests/editor_imgui_renderer_smoke.cpp
    )
    add_executable(aeg_editor_imgui_shared_texture_smoke tests/editor_imgui_shared_texture_smoke.cpp)
    target_link_libraries(aeg_editor_imgui_shared_texture_smoke PRIVATE
        aeg_editor_framework aeg_render_graph aeg_resource aeg_rhi aeg_core_shaders)
    add_executable(aeg_editor_imgui_execution_boundary_smoke
        tests/editor_imgui_execution_boundary_smoke.cpp)
    target_link_libraries(aeg_editor_imgui_execution_boundary_smoke PRIVATE aeg_editor_framework)
    target_link_libraries(aeg_editor_imgui_renderer_smoke PRIVATE
        aeg_editor_framework
        aeg_renderer
        aeg_core_shaders
    )
    target_include_directories(aeg_editor_imgui_renderer_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
    target_compile_definitions(aeg_editor_imgui_renderer_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )

    add_executable(aeg_opaque_render_feature_smoke
        tests/opaque_render_feature_smoke.cpp
    )
    add_executable(aeg_runtime_material_vulkan_smoke
        tests/runtime_material_vulkan_smoke.cpp
    )
    target_include_directories(aeg_runtime_material_vulkan_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private)
    add_executable(aeg_runtime_lighting_vulkan_smoke
        tests/runtime_lighting_vulkan_smoke.cpp
    )
    target_link_libraries(aeg_runtime_material_vulkan_smoke PRIVATE
        aeg_application_runtime
        aeg_application_tooling
        aeg_renderer
        aeg_resource
        aeg_render_graph
        aeg_rhi
        aeg_shader
        aeg_core_shaders
    )
    target_compile_definitions(aeg_runtime_material_vulkan_smoke PRIVATE
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_runtime_material_vulkan_smoke aeg_core_shaders)
    target_link_libraries(aeg_runtime_lighting_vulkan_smoke PRIVATE
        aeg_application_runtime
        aeg_resource
        aeg_render_graph
        aeg_rhi
        aeg_renderer
        aeg_shader
    )
    add_executable(aeg_renderer_depth_pass_smoke tests/renderer_depth_pass_smoke.cpp)
    target_include_directories(aeg_renderer_depth_pass_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
    add_executable(aeg_renderer_shadow_gpu_smoke tests/renderer_shadow_gpu_smoke.cpp)
    target_include_directories(aeg_renderer_shadow_gpu_smoke PRIVATE
        Engine/Renderer/private Engine/RenderGraph/private)
    add_executable(aeg_renderer_light_culling_smoke tests/renderer_light_culling_smoke.cpp)
    target_include_directories(aeg_renderer_light_culling_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
    target_link_libraries(aeg_renderer_light_culling_smoke PRIVATE aeg_renderer aeg_render_graph aeg_shader aeg_rhi aeg_core_shaders)
    target_compile_definitions(aeg_renderer_light_culling_smoke PRIVATE
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}")
    add_dependencies(aeg_renderer_light_culling_smoke aeg_core_shaders)
    target_link_libraries(aeg_renderer_shadow_gpu_smoke PRIVATE
        aeg_renderer aeg_render_graph aeg_shader aeg_rhi aeg_core_shaders aeg_application_runtime
        aeg_resource aeg_concurrency)
    target_compile_definitions(aeg_renderer_shadow_gpu_smoke PRIVATE
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}")
    add_dependencies(aeg_renderer_shadow_gpu_smoke aeg_core_shaders)
    target_link_libraries(aeg_renderer_depth_pass_smoke PRIVATE
        aeg_renderer aeg_render_graph aeg_shader aeg_rhi aeg_core_shaders aeg_application_runtime aeg_resource)
    target_compile_definitions(aeg_renderer_depth_pass_smoke PRIVATE
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}")
    add_dependencies(aeg_renderer_depth_pass_smoke aeg_core_shaders)
    target_link_libraries(aeg_opaque_render_feature_smoke PRIVATE
        aeg_engine
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_runtime
        aeg_application_tooling
        aeg_rhi
        aeg_shader
        aeg_core_shaders
    )
    target_include_directories(aeg_opaque_render_feature_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
    target_compile_definitions(aeg_opaque_render_feature_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
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
    target_include_directories(aeg_opaque_render_feature_multiframe_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private)
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
    target_include_directories(aeg_opaque_render_feature_uv_depth_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private)
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

    add_executable(aeg_editor_project_feature_authoring_smoke
        tests/editor_project_feature_authoring_smoke.cpp
        Application/Project/templates/game/Source/GameModule.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_feature_catalog_diff.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
    )
    target_include_directories(aeg_editor_project_feature_authoring_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Project/templates/game/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_project_feature_authoring_smoke PRIVATE
        aeg_editor_feature_model
        aeg_foundation
        aeg_pipeline
        aeg_module
        aeg_application_project
        aeg_application_authoring
        aeg_renderer
    )
    target_compile_definitions(aeg_editor_project_feature_authoring_smoke PRIVATE AEG_FEATURE_CONTRACT_EXPORTER=1)
    if (WIN32)
        target_compile_definitions(aeg_editor_project_feature_authoring_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

endif() # AEG_BUILD_TESTS

    add_library(aeg_editor_project_operations STATIC
        Application/Editor/private/editor_project_operations.cpp)
    target_compile_features(aeg_editor_project_operations PUBLIC cxx_std_20)
    target_link_libraries(aeg_editor_project_operations PUBLIC aeg_application_tooling PRIVATE aeg_editor_framework)
    add_executable(aeg_editor_application
        Application/Editor/private/editor_application.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Platform/private/win32/aether_editor_resources.rc
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/font/editor_system_font_catalog.cpp
    )
    target_link_libraries(aeg_editor_application PRIVATE
        aeg_editor_project_operations
        aeg_feature_runtime_host
        aeg_application_tooling
        aeg_application_platform
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_editor_font_services
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_project
        aeg_application_authoring
        aeg_editor_feature_model
        aeg_module
        aeg_rhi
        aeg_core_shaders
    )
    target_include_directories(aeg_editor_application PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
    )
    set_target_properties(aeg_editor_application PROPERTIES OUTPUT_NAME "AetherEngine")
target_compile_definitions(aeg_editor_application PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_ENGINE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        AEG_CMAKE_EXECUTABLE="${CMAKE_COMMAND}"
        AEG_EDITOR_IMGUI_SHADER_PACKAGE_PATH="${AEG_EDITOR_IMGUI_SHADER_PACKAGE}"
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_application aeg_core_shaders)
if (AEG_BUILD_TESTS)
    target_sources(aeg_editor_application PRIVATE tests/editor_main_feature_workflow_probe.cpp)
    target_include_directories(aeg_editor_application PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(aeg_editor_application PRIVATE AEG_EDITOR_WORKFLOW_TESTS=1)
    add_executable(aeg_editor_application_identity_smoke
        tests/editor_application_identity_smoke.cpp
    )
    target_compile_definitions(aeg_editor_application_identity_smoke PRIVATE
        AEG_EDITOR_APPLICATION_PATH="$<TARGET_FILE:aeg_editor_application>"
    )
    target_link_libraries(aeg_editor_application_identity_smoke PRIVATE user32 aeg_concurrency)
    add_dependencies(aeg_editor_application_identity_smoke aeg_editor_application)
    add_executable(aeg_editor_project_operations_smoke tests/editor_project_operations_smoke.cpp)
    target_include_directories(aeg_editor_project_operations_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
    target_link_libraries(aeg_editor_project_operations_smoke PRIVATE
        aeg_editor_project_operations aeg_application_tooling aeg_application_runtime aeg_editor_framework aeg_engine aeg_scene aeg_concurrency)
    add_dependencies(aeg_editor_project_operations_smoke aeg_module_dynamic_lifetime_fixture)
    add_test(NAME aeg_editor_project_operations_smoke COMMAND aeg_editor_project_operations_smoke
        $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture> "${CMAKE_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME aeg_editor_application_project_operations COMMAND "${CMAKE_COMMAND}"
        "-DEDITOR=$<TARGET_FILE:aeg_editor_application>"
        "-DFIXTURE_WRITER=$<TARGET_FILE:aeg_editor_project_operations_smoke>"
        "-DMODULE_FIXTURE=$<TARGET_FILE:aeg_module_dynamic_lifetime_fixture>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/editor-project-operations"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/editor_project_operations.cmake")
    set_tests_properties(aeg_editor_application_project_operations PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME aeg_editor_application_feature_workflow COMMAND "${CMAKE_COMMAND}"
        "-DEDITOR=$<TARGET_FILE:aeg_editor_application>"
        "-DFIXTURE_WRITER=$<TARGET_FILE:aeg_editor_project_operations_smoke>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/editor-feature-workflow"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/editor_feature_workflow.cmake")
    set_tests_properties(aeg_editor_application_feature_workflow PROPERTIES RUN_SERIAL TRUE TIMEOUT 420)
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
        PATTERN "*.aegpipe.bak" EXCLUDE
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
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
    )
    target_include_directories(aeg_editor_scene_runtime_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/DemoProject/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_scene_runtime_smoke PRIVATE
        aeg_concurrency
        aeg_editor_feature_model
        aeg_feature_runtime_host
        aeg_application_runtime
        aeg_application_tooling
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_authoring
        aeg_application_project
        aeg_module
        aeg_rhi
        aeg_core_shaders
        AEG::GameSDK
    )
    target_compile_definitions(aeg_editor_scene_runtime_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH="${AEG_OPAQUE_UNLIT_SHADER_PACKAGE}"
        AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH="${AEG_GBUFFER_MESH_SHADER_PACKAGE}"
        AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH="${AEG_DEFERRED_LIGHTING_SHADER_PACKAGE}"
        AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH="${AEG_FORWARD_SHADOW_SHADER_PACKAGE}"
        AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH="${AEG_LIGHT_CULLING_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_SHADER_PACKAGE}"
        AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH="${AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE}"
        AEG_POST_PROCESS_SHADER_PACKAGE_PATH="${AEG_POST_PROCESS_SHADER_PACKAGE}"
        AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH="${AEG_DEFERRED_SHADOW_SHADER_PACKAGE}"
        AEG_DEPTH_MESH_SHADER_PACKAGE_PATH="${AEG_DEPTH_MESH_SHADER_PACKAGE}"
        AEG_STATE_COMPOSITE_SHADER_PACKAGE_PATH="${AEG_STATE_COMPOSITE_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_scene_runtime_smoke aeg_core_shaders aeg_module_dynamic_lifetime_fixture aeg_module_dynamic_lifetime_fixture_v2 aeg_module_world_motion_fixture aeg_demo_game_module)

    get_target_property(AEG_SCENE_TEST_SOURCES aeg_editor_scene_runtime_smoke SOURCES)
    list(REMOVE_ITEM AEG_SCENE_TEST_SOURCES tests/editor_scene_runtime_smoke.cpp)
    get_target_property(AEG_SCENE_TEST_LIBRARIES aeg_editor_scene_runtime_smoke LINK_LIBRARIES)
    get_target_property(AEG_SCENE_TEST_DEFINITIONS aeg_editor_scene_runtime_smoke COMPILE_DEFINITIONS)
    add_executable(aeg_editor_renderer_pipeline_smoke tests/editor_renderer_pipeline_smoke.cpp ${AEG_SCENE_TEST_SOURCES})
    target_include_directories(aeg_editor_renderer_pipeline_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
    target_include_directories(aeg_editor_renderer_pipeline_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include)
    target_link_libraries(aeg_editor_renderer_pipeline_smoke PRIVATE ${AEG_SCENE_TEST_LIBRARIES} aeg_application_authoring aeg_concurrency)
    target_compile_definitions(aeg_editor_renderer_pipeline_smoke PRIVATE ${AEG_SCENE_TEST_DEFINITIONS})
    add_dependencies(aeg_editor_renderer_pipeline_smoke aeg_core_shaders)

    add_executable(aeg_editor_external_assets_smoke
        tests/editor_external_assets_smoke.cpp
        Application/Editor/private/editor_asset_database.cpp
        Application/Editor/private/editor_project.cpp
        Application/Editor/private/editor_scene_runtime.cpp
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
    )
    target_include_directories(aeg_editor_external_assets_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_external_assets_smoke PRIVATE
        aeg_editor_feature_model
        aeg_feature_runtime_host
        aeg_application_tooling
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_authoring
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
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
    )
    target_include_directories(aeg_editor_viewport_interaction_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_viewport_interaction_smoke PRIVATE
        aeg_editor_feature_model
        aeg_feature_runtime_host
        aeg_application_tooling
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_authoring
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
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
    )
    target_include_directories(aeg_editor_history_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_history_smoke PRIVATE
        aeg_editor_feature_model
        aeg_feature_runtime_host
        aeg_application_tooling
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_authoring
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
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
    )
    target_include_directories(aeg_editor_hierarchy_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_hierarchy_smoke PRIVATE
        aeg_editor_feature_model
        aeg_feature_runtime_host
        aeg_application_tooling
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_authoring
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
        Application/Editor/private/editor_renderer_feature_host.cpp
        Application/Editor/private/editor_project_feature_authoring.cpp
        Application/Editor/private/composition/editor_node_contract_storage.cpp
        Application/Editor/private/composition/editor_node_definition.cpp
    )
    target_include_directories(aeg_editor_inspector_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Authoring/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nlohmann/json/include
    )
    target_link_libraries(aeg_editor_inspector_smoke PRIVATE
        aeg_editor_feature_model
        aeg_feature_runtime_host
        aeg_application_tooling
        aeg_engine
        aeg_scene
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_editor_assets
        aeg_application_authoring
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

    foreach(target IN ITEMS
            aeg_editor_scene_runtime_smoke aeg_editor_renderer_pipeline_smoke
            aeg_editor_external_assets_smoke aeg_editor_viewport_interaction_smoke
            aeg_editor_history_smoke aeg_editor_hierarchy_smoke aeg_editor_inspector_smoke)
        if (TARGET ${target})
            target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/private)
        endif()
    endforeach()

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

if (TARGET aeg_editor_imgui_renderer_smoke)
    target_link_libraries(aeg_editor_imgui_renderer_smoke PRIVATE aeg_resource aeg_shader)
endif()

# Engine execution assembly is internal. Public consumers receive no legacy
# Pipeline include path; only implementation targets and native fixtures get it.
foreach(aeg_execution_impl IN ITEMS aeg_application_runtime aeg_world aeg_renderer aeg_render_graph aeg_engine aeg_editor_framework aeg_editor_application)
    if (TARGET ${aeg_execution_impl})
        target_link_libraries(${aeg_execution_impl} PRIVATE aeg_feature aeg_rhi_core aeg_concurrency)
        target_include_directories(${aeg_execution_impl} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/private
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public)
    endif()
endforeach()
if (AEG_BUILD_TESTS)
    foreach(aeg_execution_fixture IN ITEMS
        aeg_pipeline_smoke aeg_module_authoring_smoke aeg_render_pass_authoring_smoke
        aeg_pipeline_instance_configuration_smoke aeg_pipeline_runtime_smoke
        aeg_pipeline_hot_swap_smoke aeg_pipeline_phase_scheduler_smoke aeg_pipeline_value_validation_smoke aeg_frame_retention_smoke)
        target_link_libraries(${aeg_execution_fixture} PRIVATE aeg_feature)
        target_include_directories(${aeg_execution_fixture} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/private
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Feature/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Foundation/public)
    endforeach()
    get_property(aeg_native_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(aeg_native_fixture IN LISTS aeg_native_targets)
        if (NOT aeg_native_fixture MATCHES "_smoke$")
            continue()
        endif()
        get_target_property(aeg_native_fixture_dirs ${aeg_native_fixture} INCLUDE_DIRECTORIES)
        if (aeg_native_fixture_dirs MATCHES "Engine/(Renderer|RenderGraph|Runtime|World)/private")
            target_link_libraries(${aeg_native_fixture} PRIVATE aeg_feature aeg_rhi_core aeg_concurrency)
            target_include_directories(${aeg_native_fixture} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/private
                ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
                ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Concurrency/public)
        endif()
    endforeach()
endif()
if (BUILD_TESTING AND AEG_BUILD_TESTS)
    # Runtime upload and scene resolver headers are implementation seams. Keep
    # them off the target's propagated include surface and grant access only to
    # focused host tests that exercise the native upload contract directly.
    foreach(aeg_runtime_private_consumer IN ITEMS
        aeg_runtime_boot_smoke
        aeg_runtime_owned_host_boot_smoke
        aeg_runtime_scene_boot_smoke
        aeg_runtime_asset_resolution_smoke
        aeg_runtime_material_contract_smoke
        aeg_runtime_deferred_contract_smoke
        aeg_runtime_renderer_resource_contract_smoke
        aeg_runtime_object_vulkan_smoke
        aeg_runtime_lighting_vulkan_smoke
        aeg_runtime_material_vulkan_smoke
        aeg_opaque_render_feature_smoke
        aeg_renderer_depth_pass_smoke
        aeg_renderer_shadow_gpu_smoke
        aeg_feature_end_to_end_smoke
        aeg_feature_graph_runtime_e2e_smoke
        aeg_editor_application
        aeg_editor_scene_runtime_smoke
        aeg_editor_renderer_pipeline_smoke
        aeg_editor_external_assets_smoke
        aeg_editor_viewport_interaction_smoke
        aeg_editor_history_smoke
        aeg_editor_hierarchy_smoke
        aeg_editor_inspector_smoke)
        if (TARGET ${aeg_runtime_private_consumer})
            target_include_directories(${aeg_runtime_private_consumer} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/Application/Runtime/private)
        endif()
    endforeach()
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
    add_test(NAME aeg_feature_contract_smoke COMMAND aeg_feature_contract_smoke)
    add_test(NAME aeg_feature_clock_contract_smoke COMMAND aeg_feature_clock_contract_smoke)
    add_test(NAME aeg_feature_clock_admission_smoke COMMAND aeg_feature_clock_admission_smoke)
    add_test(NAME aeg_feature_clock_runtime_admission_smoke COMMAND aeg_feature_clock_runtime_admission_smoke)
    add_test(NAME aeg_feature_clock_deadline_smoke COMMAND aeg_feature_clock_deadline_smoke)
    set_tests_properties(aeg_feature_clock_deadline_smoke PROPERTIES TIMEOUT 30)
    add_test(NAME aeg_feature_outbox_smoke COMMAND aeg_feature_outbox_smoke)
    add_test(NAME aeg_feature_process_transition_order_smoke COMMAND aeg_feature_process_transition_order_smoke)
    set_tests_properties(aeg_feature_process_transition_order_smoke PROPERTIES TIMEOUT 30)
    add_test(NAME aeg_feature_runtime_smoke COMMAND aeg_feature_runtime_smoke)
    add_test(NAME aeg_feature_asset_parameters_smoke COMMAND aeg_feature_asset_parameters_smoke)
    add_test(NAME aeg_feature_runtime_regression_smoke COMMAND aeg_feature_runtime_regression_smoke)
    add_test(NAME aeg_feature_async_execution_smoke COMMAND aeg_feature_async_execution_smoke)
    set_tests_properties(aeg_feature_async_execution_smoke PROPERTIES TIMEOUT 30)
    add_test(NAME aeg_feature_transport_smoke COMMAND aeg_feature_transport_smoke)
    add_test(NAME aeg_feature_transport_semantics_smoke COMMAND aeg_feature_transport_semantics_smoke)
    add_test(NAME aeg_feature_graph_authoring_smoke COMMAND aeg_feature_graph_authoring_smoke)
    if (TARGET aeg_editor_feature_graph_smoke)
        add_test(NAME aeg_editor_feature_graph_smoke COMMAND aeg_editor_feature_graph_smoke)
        add_test(NAME aeg_editor_feature_graph_preview_smoke COMMAND aeg_editor_feature_graph_preview_smoke)
        add_test(NAME aeg_editor_feature_palette_clock_smoke COMMAND aeg_editor_feature_palette_clock_smoke)
        add_test(NAME aeg_editor_feature_palette_clock_ui_smoke COMMAND aeg_editor_feature_palette_clock_ui_smoke)
        add_test(NAME aeg_editor_feature_edge_constraints_ui_smoke COMMAND aeg_editor_feature_edge_constraints_ui_smoke)
        add_test(NAME aeg_editor_feature_graph_native_scroll_smoke COMMAND aeg_editor_feature_graph_native_scroll_smoke)
        add_test(NAME aeg_feature_execution_inspection_smoke COMMAND aeg_feature_execution_inspection_smoke)
        add_test(NAME aeg_editor_feature_execution_ui_smoke COMMAND aeg_editor_feature_execution_ui_smoke)
        add_test(NAME aeg_editor_process_canvas_smoke COMMAND aeg_editor_process_canvas_smoke)
        add_test(NAME aeg_editor_feature_graph_canvas_ui_smoke COMMAND aeg_editor_feature_graph_canvas_ui_smoke)
        add_test(NAME aeg_editor_feature_graph_orchestration_ui_smoke COMMAND aeg_editor_feature_graph_orchestration_ui_smoke)
        add_test(NAME aeg_editor_feature_graph_transactions_smoke COMMAND aeg_editor_feature_graph_transactions_smoke)
        add_test(NAME aeg_editor_feature_implementation_resolver_smoke COMMAND aeg_editor_feature_implementation_resolver_smoke)
        add_test(NAME aeg_editor_feature_definition_navigation_ui_smoke COMMAND aeg_editor_feature_definition_navigation_ui_smoke)
        add_test(NAME aeg_editor_process_layout_smoke COMMAND aeg_editor_process_layout_smoke)
    endif()
    add_test(NAME aeg_feature_graph_runtime_e2e_smoke COMMAND aeg_feature_graph_runtime_e2e_smoke)
    add_test(NAME aeg_feature_graph_document_io_smoke COMMAND aeg_feature_graph_document_io_smoke)
    add_test(NAME aeg_feature_graph_source_crash_smoke
        COMMAND ${CMAKE_COMMAND}
            -DPROBE=$<TARGET_FILE:aeg_feature_graph_document_io_smoke>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/feature-graph-source-crash
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/feature_graph_source_crash.cmake)
    add_test(NAME aeg_feature_contract_exporter_smoke
        COMMAND ${CMAKE_COMMAND}
            -DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/feature-contract-exporter
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/feature_contract_exporter.cmake)
    set_tests_properties(aeg_feature_contract_exporter_smoke PROPERTIES TIMEOUT 900)
    add_test(NAME aeg_feature_module_publication_smoke
        COMMAND ${CMAKE_COMMAND}
            -DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/feature-module-publication
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/feature_module_publication.cmake)
    set_tests_properties(aeg_feature_module_publication_smoke PROPERTIES TIMEOUT 1200)
    add_test(NAME aeg_configuration_idempotence_smoke
        COMMAND ${CMAKE_COMMAND}
            -DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/configuration-idempotence
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/configuration_idempotence.cmake)
    add_test(NAME aeg_feature_authoring_boundary_smoke COMMAND aeg_feature_authoring_boundary_smoke)
    if (TARGET aeg_feature_graph_editor_layout_cook_smoke)
        add_test(NAME aeg_feature_graph_editor_layout_cook_smoke COMMAND aeg_feature_graph_editor_layout_cook_smoke)
    endif()
    add_test(NAME aeg_feature_render_projection_smoke COMMAND aeg_feature_render_projection_smoke)
    add_test(NAME aeg_feature_render_execution_smoke COMMAND aeg_feature_render_execution_smoke)
    add_test(NAME aeg_feature_render_history_smoke COMMAND aeg_feature_render_history_smoke)
    add_test(NAME aeg_render_feature_history_boundary_smoke
        COMMAND aeg_render_feature_history_boundary_smoke)
    set_tests_properties(aeg_feature_render_history_smoke PROPERTIES TIMEOUT 30
        FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
    set_tests_properties(aeg_feature_render_execution_smoke PROPERTIES TIMEOUT 30
        FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
    add_test(NAME aeg_feature_no_legacy_identity_smoke COMMAND aeg_feature_no_legacy_identity_smoke)
    set_tests_properties(aeg_feature_no_legacy_identity_smoke PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME aeg_module_feature_registration_smoke COMMAND aeg_module_feature_registration_smoke)
    add_test(NAME aeg_feature_module_authoring_smoke COMMAND aeg_feature_module_authoring_smoke)
    add_test(NAME aeg_feature_module_header_only_smoke COMMAND aeg_feature_module_header_only_smoke)
    if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
    add_test(NAME aeg_asset_metadata_smoke COMMAND aeg_asset_metadata_smoke)
    add_test(NAME aeg_project_operation_smoke COMMAND aeg_project_operation_smoke
        $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture> "${CMAKE_COMMAND}")
    add_test(NAME aeg_project_cooker_smoke COMMAND aeg_project_cooker_smoke $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture>)
    add_test(NAME aeg_runtime_asset_package_smoke COMMAND aeg_runtime_asset_package_smoke)
    add_test(NAME aeg_runtime_asset_resolution_smoke COMMAND aeg_runtime_asset_resolution_smoke)
    add_test(NAME aeg_runtime_font_cache_smoke COMMAND aeg_runtime_font_cache_smoke)
    add_test(NAME aeg_shader_contract_smoke COMMAND aeg_shader_contract_smoke)
    add_test(NAME aeg_shader_cache_epoch_smoke COMMAND aeg_shader_cache_epoch_smoke)
    add_test(NAME aeg_runtime_material_contract_smoke COMMAND aeg_runtime_material_contract_smoke)
    add_test(NAME aeg_runtime_deferred_contract_smoke COMMAND aeg_runtime_deferred_contract_smoke)
    add_test(NAME aeg_runtime_renderer_resource_contract_smoke
        COMMAND aeg_runtime_renderer_resource_contract_smoke)
    add_test(NAME aeg_runtime_object_vulkan_smoke COMMAND aeg_runtime_object_vulkan_smoke)
    add_test(NAME aeg_game_build_manifest_smoke COMMAND aeg_game_build_manifest_smoke)
    add_test(NAME aeg_world_command_routes_smoke COMMAND aeg_world_command_routes_smoke)
    add_test(NAME aeg_runtime_boot_smoke COMMAND aeg_runtime_boot_smoke)
    add_test(NAME aeg_runtime_world_commands_smoke COMMAND aeg_runtime_world_commands_smoke)
    add_test(NAME aeg_runtime_world_observation_smoke COMMAND aeg_runtime_world_commands_smoke --motion-observation)
    add_test(NAME aeg_runtime_world_motion_smoke COMMAND aeg_runtime_world_commands_smoke --motion-observation-ordinary)
    add_test(NAME aeg_runtime_world_motion_loop_smoke COMMAND aeg_runtime_world_commands_smoke --motion-run-loop)
    add_test(NAME aeg_world_transform_protocol_smoke COMMAND aeg_world_transform_protocol_smoke)
    add_test(NAME aeg_feature_host_input_smoke COMMAND aeg_feature_host_input_smoke)
    set_tests_properties(aeg_runtime_world_commands_smoke PROPERTIES
        FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
    set_tests_properties(aeg_runtime_boot_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
    add_test(NAME aeg_runtime_scene_boot_smoke COMMAND aeg_runtime_scene_boot_smoke)
    add_test(NAME aeg_runtime_window_host_smoke COMMAND aeg_runtime_window_host_smoke)
    set_tests_properties(aeg_runtime_window_host_smoke PROPERTIES RESOURCE_LOCK runtime_task1_fixture)
    if (WIN32 AND AEG_BUILD_EDITOR)
        add_test(NAME aeg_runtime_owned_host_boot_smoke COMMAND aeg_runtime_owned_host_boot_smoke)
        add_test(NAME aeg_runtime_owned_host_no_scene_smoke COMMAND aeg_runtime_owned_host_boot_smoke --no-scene)
        add_test(NAME aeg_runtime_owned_host_depth_smoke COMMAND aeg_runtime_owned_host_boot_smoke --depth)
        add_test(NAME aeg_runtime_owned_host_depth_only_smoke COMMAND aeg_runtime_owned_host_boot_smoke --depth-only)
        add_test(NAME aeg_runtime_owned_host_shadow_depth_smoke COMMAND aeg_runtime_owned_host_boot_smoke --shadow-depth)
        add_test(NAME aeg_runtime_owned_host_shadow_draw_smoke COMMAND aeg_runtime_owned_host_boot_smoke --shadow-draw)
        add_test(NAME aeg_runtime_owned_host_gbuffer_smoke COMMAND aeg_runtime_owned_host_boot_smoke --gbuffer)
        add_test(NAME aeg_runtime_owned_host_deferred_lighting_smoke COMMAND aeg_runtime_owned_host_boot_smoke --deferred-lighting)
        add_test(NAME aeg_runtime_owned_host_post_process_smoke COMMAND aeg_runtime_owned_host_boot_smoke --feature-post-process)
        add_test(NAME aeg_runtime_owned_host_clustered_publication_smoke
            COMMAND aeg_runtime_owned_host_boot_smoke --feature-clustered-publication)
        add_test(NAME aeg_runtime_owned_host_clustered_process_smoke
            COMMAND aeg_runtime_owned_host_boot_smoke --feature-clustered-process)
        add_test(NAME aeg_runtime_owned_host_clustered_cancel_smoke
            COMMAND aeg_runtime_owned_host_boot_smoke --feature-clustered-cancel)
        add_test(NAME aeg_runtime_owned_host_clustered_shadow_publication_smoke
            COMMAND aeg_runtime_owned_host_boot_smoke --feature-clustered-shadow-publication)
        add_test(NAME aeg_runtime_owned_host_clustered_shadow_process_smoke
            COMMAND aeg_runtime_owned_host_boot_smoke --feature-clustered-shadow-process)
        add_test(NAME aeg_runtime_owned_host_clustered_shadow_cancel_smoke
            COMMAND aeg_runtime_owned_host_boot_smoke --feature-clustered-shadow-cancel)
        set_tests_properties(aeg_runtime_owned_host_boot_smoke aeg_runtime_owned_host_no_scene_smoke
            aeg_runtime_owned_host_depth_smoke aeg_runtime_owned_host_depth_only_smoke aeg_runtime_owned_host_shadow_depth_smoke
            aeg_runtime_owned_host_shadow_draw_smoke aeg_runtime_owned_host_gbuffer_smoke
            aeg_runtime_owned_host_deferred_lighting_smoke aeg_runtime_owned_host_post_process_smoke
            aeg_runtime_owned_host_clustered_publication_smoke aeg_runtime_owned_host_clustered_process_smoke
            aeg_runtime_owned_host_clustered_cancel_smoke aeg_runtime_owned_host_clustered_shadow_publication_smoke
            aeg_runtime_owned_host_clustered_shadow_process_smoke aeg_runtime_owned_host_clustered_shadow_cancel_smoke PROPERTIES TIMEOUT 120
            RESOURCE_LOCK runtime_task1_fixture
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
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
    add_test(NAME aeg_concurrency_boundary
        COMMAND ${CMAKE_COMMAND}
            -DAEG_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/concurrency_boundary.cmake)
    add_test(NAME aeg_game_sdk_boundary_smoke COMMAND aeg_game_sdk_boundary_smoke)
    add_test(NAME aeg_runtime_feature_inspect_boundary_smoke COMMAND aeg_runtime_feature_inspect_boundary_smoke)
    add_test(NAME aeg_runtime_upload_host_boundary_smoke COMMAND aeg_runtime_upload_host_boundary_smoke)
    if (TARGET aeg_editor_generated_feature_sdk_smoke)
        add_test(NAME aeg_editor_generated_feature_sdk_smoke COMMAND aeg_editor_generated_feature_sdk_smoke)
    endif()
    add_test(NAME aeg_concurrency_smoke COMMAND aeg_concurrency_smoke)
    add_test(NAME aeg_ecs_smoke COMMAND aeg_ecs_smoke)
    add_test(NAME aeg_pipeline_smoke COMMAND aeg_pipeline_smoke)
    add_test(NAME aeg_module_authoring_smoke COMMAND aeg_module_authoring_smoke)
    add_test(NAME aeg_render_pass_authoring_smoke COMMAND aeg_render_pass_authoring_smoke)
    add_test(NAME aeg_pipeline_instance_configuration_smoke COMMAND aeg_pipeline_instance_configuration_smoke)
    add_test(NAME aeg_pipeline_runtime_smoke COMMAND aeg_pipeline_runtime_smoke)
    add_test(NAME aeg_pipeline_hot_swap_smoke COMMAND aeg_pipeline_hot_swap_smoke)
    add_test(NAME aeg_pipeline_phase_scheduler_smoke COMMAND aeg_pipeline_phase_scheduler_smoke)
    add_test(NAME aeg_module_smoke COMMAND aeg_module_smoke)
    add_test(NAME aeg_module_abi_header COMMAND aeg_module_abi_header)
    add_test(NAME aeg_module_abi_smoke COMMAND aeg_module_abi_smoke)
    add_test(NAME aeg_module_feature_callback_codec_smoke COMMAND aeg_module_feature_callback_codec_smoke)
    add_test(NAME aeg_module_metadata_smoke COMMAND aeg_module_metadata_smoke)
    add_test(NAME aeg_module_dynamic_lifetime_smoke COMMAND aeg_module_dynamic_lifetime_smoke
        $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture> $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture_v2>)
    if (TARGET aeg_feature_end_to_end_smoke)
        add_test(NAME aeg_feature_end_to_end_smoke COMMAND aeg_feature_end_to_end_smoke
            $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture>)
        set_tests_properties(aeg_feature_end_to_end_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
    endif()
    add_test(NAME aeg_module_context_table_smoke COMMAND aeg_module_context_table_smoke)
    if (AEG_BUILD_TOOLS OR AEG_BUILD_EDITOR)
        add_test(NAME aeg_editor_composition_catalog_smoke COMMAND aeg_editor_composition_catalog_smoke)
        add_test(NAME aeg_editor_process_layout_boundary_smoke
            COMMAND aeg_editor_process_layout_boundary_smoke)
        add_test(NAME aeg_editor_composition_boundary_smoke
            COMMAND aeg_editor_composition_boundary_smoke)
        add_test(NAME aeg_editor_custom_node_workflow_smoke COMMAND aeg_editor_custom_node_workflow_smoke)
        add_test(NAME aeg_editor_node_definition_model_smoke COMMAND aeg_editor_node_definition_model_smoke)
        add_test(NAME aeg_editor_feature_definition_ui_smoke COMMAND aeg_editor_feature_definition_ui_smoke)
        add_test(NAME aeg_editor_generated_project_module
            COMMAND ${CMAKE_COMMAND}
                -DFIXTURE=$<TARGET_FILE:aeg_editor_generated_project_module_smoke>
                -DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/editor-generated-project-module
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/editor_generated_project_module.cmake)
        set_tests_properties(aeg_editor_generated_project_module PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
        add_test(NAME aeg_editor_node_contract_storage_smoke COMMAND aeg_editor_node_contract_storage_smoke)
        add_test(NAME aeg_editor_node_definition_generation_smoke COMMAND aeg_editor_node_definition_generation_smoke)
        add_test(NAME aeg_editor_node_definition_service_smoke COMMAND aeg_editor_node_definition_service_smoke)
        add_test(NAME aeg_editor_node_definition_end_to_end_smoke COMMAND aeg_editor_node_definition_end_to_end_smoke)
    endif()
    add_test(NAME aeg_world_scope_smoke COMMAND aeg_world_scope_smoke)
    add_test(NAME aeg_world_public_boundary_smoke COMMAND aeg_world_public_boundary_smoke)
    add_test(NAME aeg_engine_public_boundary_smoke COMMAND aeg_engine_public_boundary_smoke)
    add_test(NAME aeg_frame_task_owner_boundary_smoke COMMAND aeg_frame_task_owner_boundary_smoke)
    add_test(NAME aeg_pipeline_registration_owner_boundary_smoke COMMAND aeg_pipeline_registration_owner_boundary_smoke)
    add_test(NAME aeg_pipeline_value_validation_smoke COMMAND aeg_pipeline_value_validation_smoke)
    add_test(NAME aeg_render_graph_pass_inspection_smoke COMMAND aeg_render_graph_pass_inspection_smoke)
    add_test(NAME aeg_world_serialization_smoke COMMAND aeg_world_serialization_smoke)
    add_test(NAME aeg_scene_asset_smoke COMMAND aeg_scene_asset_smoke)
    add_test(NAME aeg_world_frame_orchestrator_smoke COMMAND aeg_world_frame_orchestrator_smoke)
    add_test(NAME aeg_world_feature_commands_smoke COMMAND aeg_world_feature_commands_smoke)
    add_test(NAME aeg_world_raw_pipeline_rejection_smoke COMMAND aeg_world_raw_pipeline_rejection_smoke)
    add_test(NAME aeg_world_multi_view_smoke COMMAND aeg_world_multi_view_smoke)
    add_test(NAME aeg_engine_frame_orchestrator_smoke COMMAND aeg_engine_frame_orchestrator_smoke)
    add_test(NAME aeg_runtime_components_scene_smoke COMMAND aeg_runtime_components_scene_smoke)
    add_test(NAME aeg_runtime_component_orchestrator_smoke COMMAND aeg_runtime_component_orchestrator_smoke)
    add_test(NAME aeg_renderer_smoke COMMAND aeg_renderer_smoke)
    add_test(NAME aeg_renderer_feature_context_boundary_smoke
        COMMAND aeg_renderer_feature_context_boundary_smoke)
    add_test(NAME aeg_renderer_shader_contract_boundary_smoke
        COMMAND aeg_renderer_shader_contract_boundary_smoke)
    add_test(NAME aeg_renderer_profile_contract_smoke
        COMMAND aeg_renderer_profile_contract_smoke)
    add_test(NAME aeg_renderer_pipeline_variants_smoke COMMAND aeg_renderer_pipeline_variants_smoke)
    add_test(NAME aeg_renderer_builtin_pass_smoke COMMAND aeg_renderer_builtin_pass_smoke)
    add_test(NAME aeg_renderer_shadow_view_smoke COMMAND aeg_renderer_shadow_view_smoke)
    add_test(NAME aeg_renderer_authoring_smoke COMMAND aeg_renderer_authoring_smoke)
    add_test(NAME aeg_renderer_feature_profiles_smoke COMMAND aeg_renderer_feature_profiles_smoke)
    add_test(NAME aeg_editor_renderer_feature_host_smoke COMMAND aeg_editor_renderer_feature_host_smoke)
    add_test(NAME aeg_renderer_pipeline_graph_smoke COMMAND aeg_renderer_pipeline_graph_smoke)
    set_tests_properties(aeg_renderer_pipeline_graph_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME aeg_frame_retention_smoke COMMAND aeg_frame_retention_smoke)
    add_test(NAME aeg_render_graph_context_contract_smoke COMMAND aeg_render_graph_context_contract_smoke)
    add_test(NAME aeg_render_graph_imported_texture_state_smoke COMMAND aeg_render_graph_imported_texture_state_smoke)
    add_test(NAME aeg_render_graph_imported_buffer_smoke COMMAND aeg_render_graph_imported_buffer_smoke)
    set_tests_properties(aeg_render_graph_imported_buffer_smoke PROPERTIES
        FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
    add_test(NAME aeg_rhi_smoke COMMAND aeg_rhi_smoke)
    add_test(NAME aeg_logging_smoke COMMAND aeg_logging_smoke)
    if (WIN32 AND AEG_BUILD_EDITOR)
        add_test(NAME aeg_triangle COMMAND aeg_triangle --frames 1)
    endif()
    if (WIN32 AND AEG_BUILD_EDITOR)
        add_test(NAME aeg_editor_imgui_renderer_smoke COMMAND aeg_editor_imgui_renderer_smoke)
        add_test(NAME aeg_editor_imgui_shared_texture_smoke COMMAND aeg_editor_imgui_shared_texture_smoke)
        add_test(NAME aeg_editor_imgui_execution_boundary_smoke COMMAND aeg_editor_imgui_execution_boundary_smoke)
        add_test(NAME aeg_opaque_render_feature_smoke COMMAND aeg_opaque_render_feature_smoke)
        add_test(NAME aeg_feature_scene_draw_smoke COMMAND aeg_opaque_render_feature_smoke --neutral)
        add_test(NAME aeg_feature_registered_scene_draw_smoke COMMAND aeg_opaque_render_feature_smoke --neutral-registered)
        add_test(NAME aeg_feature_scene_draw_clear_smoke COMMAND aeg_opaque_render_feature_smoke --neutral-clear)
        add_test(NAME aeg_feature_scene_draw_cold_smoke COMMAND aeg_opaque_render_feature_smoke --neutral-cold)
        add_test(NAME aeg_feature_scene_draw_retry_smoke COMMAND aeg_opaque_render_feature_smoke --neutral-retry)
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
        add_test(NAME aeg_clustered_shader_reflection_smoke COMMAND aeg_renderer_shadow_gpu_smoke --clustered-reflection)
        add_test(NAME aeg_clustered_unshadowed_smoke COMMAND aeg_renderer_shadow_gpu_smoke --clustered-unshadowed)
        add_test(NAME aeg_feature_clustered_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered)
        add_test(NAME aeg_feature_clustered_shadow_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-shadow)
        set_tests_properties(aeg_feature_clustered_shadow_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_feature_clustered_shadow_publication_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-shadow-publication)
        add_test(NAME aeg_feature_clustered_shadow_process_suspend_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-shadow-process-suspend)
        add_test(NAME aeg_feature_clustered_shadow_process_cancel_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-shadow-process-cancel)
        set_tests_properties(aeg_feature_clustered_shadow_publication_smoke aeg_feature_clustered_shadow_process_suspend_smoke
            aeg_feature_clustered_shadow_process_cancel_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_feature_clustered_two_producers_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-two-producers)
        set_tests_properties(aeg_feature_clustered_two_producers_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_feature_clustered_publication_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-publication)
        add_test(NAME aeg_feature_clustered_process_suspend_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-process-suspend)
        add_test(NAME aeg_feature_clustered_process_cancel_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-clustered-process-cancel)
        set_tests_properties(aeg_feature_clustered_process_suspend_smoke aeg_feature_clustered_process_cancel_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_feature_clustered_publication_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_feature_clustered_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_clustered_unshadowed_smoke PROPERTIES FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_feature_scene_shadow_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-intent)
        add_test(NAME aeg_feature_scene_gbuffer_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-gbuffer)
        add_test(NAME aeg_feature_deferred_lighting_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-deferred)
        add_test(NAME aeg_feature_post_process_smoke COMMAND aeg_renderer_shadow_gpu_smoke --feature-post-process)
        set_tests_properties(aeg_feature_scene_gbuffer_smoke aeg_feature_deferred_lighting_smoke aeg_feature_post_process_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
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
        set_tests_properties(aeg_forward_shadow_smoke aeg_deferred_shadow_smoke aeg_feature_scene_shadow_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        set_tests_properties(aeg_renderer_depth_pass_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_opaque_render_feature_multiframe_smoke COMMAND aeg_opaque_render_feature_multiframe_smoke)
        add_test(NAME aeg_opaque_render_feature_uv_depth_smoke COMMAND aeg_opaque_render_feature_uv_depth_smoke)
        add_test(NAME aeg_feature_scene_depth_load_smoke COMMAND aeg_opaque_render_feature_uv_depth_smoke --depth-load)
        add_test(NAME aeg_feature_scene_depth_clear_smoke COMMAND aeg_opaque_render_feature_uv_depth_smoke --depth-clear-near)
        add_test(NAME aeg_editor_scene_runtime_smoke COMMAND aeg_editor_scene_runtime_smoke
            $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture> $<TARGET_FILE:aeg_module_dynamic_lifetime_fixture_v2>)
        add_test(NAME aeg_editor_world_api_smoke COMMAND aeg_editor_scene_runtime_smoke --world-api)
        add_test(NAME aeg_editor_world_binding_smoke COMMAND aeg_editor_scene_runtime_smoke --world-feature
            $<TARGET_FILE:aeg_module_world_motion_fixture>)
        add_test(NAME aeg_editor_demo_motion_smoke COMMAND aeg_editor_scene_runtime_smoke --demo-motion
            $<TARGET_FILE:aeg_demo_game_module> "${CMAKE_CURRENT_SOURCE_DIR}/DemoProject")
        add_test(NAME aeg_editor_renderer_pipeline_smoke COMMAND aeg_editor_renderer_pipeline_smoke)
        set_tests_properties(aeg_editor_renderer_pipeline_smoke PROPERTIES
            FAIL_REGULAR_EXPRESSION "Vulkan validation \\[ERROR\\]")
        add_test(NAME aeg_editor_project_feature_authoring_smoke COMMAND aeg_editor_project_feature_authoring_smoke)
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
                 COMMAND aeg_editor_application --frames 2 --project "${CMAKE_CURRENT_BINARY_DIR}/missing-recovery-project/Project.aegproj")
        # Win32 focus/activation is a shared desktop resource, including the
        # native Runtime host fixtures already protected by this resource.
        set_tests_properties(aeg_application_platform_win32_smoke
            aeg_editor_application_identity_smoke aeg_editor_application
            aeg_editor_application_preferences aeg_editor_application_native_viewports
            aeg_editor_application_missing_project
            PROPERTIES RESOURCE_LOCK runtime_task1_fixture)
        set_tests_properties(aeg_editor_imgui_renderer_smoke aeg_opaque_render_feature_smoke aeg_feature_scene_draw_smoke aeg_feature_registered_scene_draw_smoke aeg_feature_scene_draw_clear_smoke
                aeg_feature_scene_draw_cold_smoke aeg_feature_scene_draw_retry_smoke
                aeg_opaque_render_feature_multiframe_smoke aeg_opaque_render_feature_uv_depth_smoke
                aeg_feature_scene_depth_load_smoke aeg_feature_scene_depth_clear_smoke
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
                aeg_pipeline_runtime_smoke
                aeg_pipeline_hot_swap_smoke aeg_pipeline_phase_scheduler_smoke aeg_module_smoke aeg_world_scope_smoke
                aeg_world_serialization_smoke aeg_scene_asset_smoke aeg_world_frame_orchestrator_smoke
                aeg_world_multi_view_smoke aeg_engine_frame_orchestrator_smoke aeg_runtime_components_scene_smoke
                aeg_runtime_component_orchestrator_smoke aeg_renderer_smoke aeg_opaque_render_feature_smoke
                aeg_opaque_render_feature_multiframe_smoke aeg_opaque_render_feature_uv_depth_smoke
                aeg_editor_scene_runtime_smoke aeg_editor_external_assets_smoke aeg_editor_viewport_interaction_smoke
                aeg_editor_history_smoke aeg_editor_hierarchy_smoke aeg_editor_inspector_smoke
                aeg_editor_asset_database_smoke aeg_editor_project_smoke
                aeg_frame_retention_smoke aeg_rhi_smoke aeg_logging_smoke aeg_triangle
                aeg_editor_application_identity_smoke aeg_editor_application aeg_editor_application_preferences
                aeg_editor_application_native_viewports
                aeg_editor_application_missing_project aeg_editor_error_smoke aeg_editor_asset_pipeline_recovery_smoke
                aeg_render_graph_imported_texture_state_smoke aeg_render_graph_imported_buffer_smoke)
    endif()
endif()
