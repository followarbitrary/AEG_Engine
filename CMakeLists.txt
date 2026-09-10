cmake_minimum_required(VERSION 3.25)
project(AEG_Engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/Zc:preprocessor)
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
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(AEG_LOG_COMPILE_LEVEL_VALUE 3)
    elseif (CMAKE_BUILD_TYPE STREQUAL "Release" OR
           CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR
           CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(AEG_LOG_COMPILE_LEVEL_VALUE 4)
    else()
        set(AEG_LOG_COMPILE_LEVEL_VALUE -1)
    endif()
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

if (WIN32)
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

add_executable(aeg_math_smoke
    tests/math_smoke.cpp
)
target_link_libraries(aeg_math_smoke PRIVATE aeg_math)

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

if (WIN32)
    add_library(aeg_application_platform_win32 STATIC
        Application/Platform/private/win32/win32_window.cpp
        Application/Platform/private/win32/win32_window_system.cpp
    )
    target_compile_features(aeg_application_platform_win32 PUBLIC cxx_std_20)
    target_include_directories(aeg_application_platform_win32
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/public
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Application/Platform/private/win32
    )
    target_compile_definitions(aeg_application_platform_win32
        PRIVATE
            NOMINMAX
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(aeg_application_platform_win32 PUBLIC aeg_platform aeg_input user32 imm32)

    if (MSVC)
        target_compile_options(aeg_application_platform_win32 PRIVATE /W4 /permissive-)
    else()
        target_compile_options(aeg_application_platform_win32 PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(aeg_application_platform_win32_smoke
        tests/application_platform_win32_smoke.cpp
    )
    target_link_libraries(aeg_application_platform_win32_smoke PRIVATE aeg_application_platform_win32)
    target_compile_definitions(aeg_application_platform_win32_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

add_library(aeg_editor_framework STATIC
    Engine/EditorFramework/private/imgui/editor_imgui_host.cpp
    Engine/EditorFramework/private/imgui/imgui_platform_bridge.cpp
    Engine/EditorFramework/private/imgui/imgui_texture_registry.cpp
)
target_compile_features(aeg_editor_framework PUBLIC cxx_std_20)
target_include_directories(aeg_editor_framework
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/EditorFramework/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Input/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
    PRIVATE
        ${AEG_IMGUI_DIR}
)
target_link_libraries(aeg_editor_framework PUBLIC aeg_foundation aeg_platform aeg_input)
target_link_libraries(aeg_editor_framework PUBLIC aeg_imgui_core)

if (MSVC)
    target_compile_options(aeg_editor_framework PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_editor_framework PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_editor_imgui_smoke
    tests/editor_imgui_smoke.cpp
)
target_link_libraries(aeg_editor_imgui_smoke PRIVATE aeg_editor_framework)
if (WIN32)
    target_compile_definitions(aeg_editor_imgui_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_result_smoke
    tests/result_smoke.cpp
)
target_link_libraries(aeg_result_smoke PRIVATE aeg_foundation)
if (WIN32)
    target_compile_definitions(aeg_result_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

add_executable(aeg_reflection_smoke
    tests/reflection_smoke.cpp
)
target_link_libraries(aeg_reflection_smoke PRIVATE aeg_reflection)
if (WIN32)
    target_compile_definitions(aeg_reflection_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

add_executable(aeg_serialization_smoke
    tests/serialization_smoke.cpp
)
target_link_libraries(aeg_serialization_smoke PRIVATE aeg_serialization)
if (WIN32)
    target_compile_definitions(aeg_serialization_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

add_executable(aeg_concurrency_smoke
    tests/concurrency_smoke.cpp
)
target_link_libraries(aeg_concurrency_smoke PRIVATE aeg_concurrency)
if (WIN32)
    target_compile_definitions(aeg_concurrency_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

add_executable(aeg_ecs_smoke
    tests/ecs_smoke.cpp
)
target_link_libraries(aeg_ecs_smoke PRIVATE aeg_ecs)
if (WIN32)
    target_compile_definitions(aeg_ecs_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(aeg_pipeline STATIC
    Engine/Pipeline/private/pipeline.cpp
)
target_compile_features(aeg_pipeline PUBLIC cxx_std_20)
target_include_directories(aeg_pipeline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
)
target_link_libraries(aeg_pipeline PUBLIC aeg_foundation aeg_concurrency Threads::Threads)

if (MSVC)
    target_compile_options(aeg_pipeline PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

add_library(aeg_module STATIC
    Engine/Module/private/module.cpp
)
target_compile_features(aeg_module PUBLIC cxx_std_20)
target_include_directories(aeg_module
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Module/public
)
target_link_libraries(aeg_module PUBLIC aeg_foundation aeg_pipeline aeg_reflection)

if (MSVC)
    target_compile_options(aeg_module PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_module PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(aeg_module_smoke
    tests/module_smoke.cpp
)
target_link_libraries(aeg_module_smoke PRIVATE aeg_module)
if (WIN32)
    target_compile_definitions(aeg_module_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

add_library(aeg_scene STATIC
    Engine/Scene/private/scene_asset.cpp
)
target_compile_features(aeg_scene PUBLIC cxx_std_20)
target_include_directories(aeg_scene
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Scene/public
)
target_link_libraries(aeg_scene PUBLIC aeg_world aeg_serialization)

if (MSVC)
    target_compile_options(aeg_scene PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_scene PRIVATE -Wall -Wextra -Wpedantic)
endif()

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

add_library(aeg_renderer STATIC
    Engine/Renderer/private/renderer.cpp
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

add_executable(aeg_engine_frame_orchestrator_smoke
    tests/engine_frame_orchestrator_smoke.cpp
)
target_link_libraries(aeg_engine_frame_orchestrator_smoke PRIVATE aeg_engine aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_engine_frame_orchestrator_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_renderer_smoke
    tests/renderer_smoke.cpp
)
target_link_libraries(aeg_renderer_smoke PRIVATE aeg_engine aeg_renderer aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_renderer_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(aeg_render_feature_runtime_smoke
    tests/render_feature_runtime_smoke.cpp
)
add_executable(aeg_frame_retention_smoke tests/frame_retention_smoke.cpp)
target_link_libraries(aeg_frame_retention_smoke PRIVATE aeg_render_graph aeg_rhi)
target_link_libraries(aeg_render_feature_runtime_smoke PRIVATE aeg_render_graph aeg_rhi)
if (WIN32)
    target_compile_definitions(aeg_render_feature_runtime_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

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

target_sources(aeg_editor_framework PRIVATE
    Engine/EditorFramework/private/imgui/imgui_renderer.cpp
)
target_include_directories(aeg_editor_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Math/public
)
target_link_libraries(aeg_editor_framework PUBLIC aeg_resource aeg_rhi aeg_shader)

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

if (WIN32)
    add_executable(aeg_shader_contract_tool
        tools/shader_contract_tool.cpp
    )
    target_compile_features(aeg_shader_contract_tool PRIVATE cxx_std_20)
    target_include_directories(aeg_shader_contract_tool
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Shader/public
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
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/public")
    set(AEG_SHADER_GENERATED_INCLUDE_DIR
        "${AEG_SHADER_PUBLIC_DIR}/generated")
    set(AEG_SHADER_GENERATED_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/private/generated")
    set(AEG_SHADER_NAMESPACE
        aeg::shaders)
    set(AEG_COMMON_SHADER_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common")
    set(AEG_CORE_SHADER_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/core")
    set(AEG_TEST_SHADER_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")

    add_library(aeg_core_shaders STATIC)
    target_compile_features(aeg_core_shaders PUBLIC cxx_std_20)
    target_include_directories(aeg_core_shaders
        PUBLIC
            ${AEG_SHADER_PUBLIC_DIR}
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
    target_compile_definitions(aeg_editor_framework PRIVATE
        AEG_EDITOR_IMGUI_SHADER_PACKAGE_PATH="${AEG_EDITOR_IMGUI_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_framework aeg_core_shaders)

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

    add_executable(aeg_editor_application
        Application/Editor/private/editor_application.cpp
    )
    target_link_libraries(aeg_editor_application PRIVATE
        aeg_application_platform_win32
        aeg_engine
        aeg_editor_framework
        aeg_renderer
        aeg_resource
        aeg_rhi
        aeg_core_shaders
    )
    target_compile_definitions(aeg_editor_application PRIVATE
        _CRT_SECURE_NO_WARNINGS
        AEG_EDITOR_IMGUI_SHADER_PACKAGE_PATH="${AEG_EDITOR_IMGUI_SHADER_PACKAGE}"
    )
    add_dependencies(aeg_editor_application aeg_core_shaders)

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
endif()

if (BUILD_TESTING)
    add_test(NAME aeg_math_smoke COMMAND aeg_math_smoke)
    add_test(NAME aeg_result_smoke COMMAND aeg_result_smoke)
    add_test(NAME aeg_editor_imgui_smoke COMMAND aeg_editor_imgui_smoke)
    if (WIN32)
        add_test(NAME aeg_application_platform_win32_smoke COMMAND aeg_application_platform_win32_smoke)
    endif()
    add_test(NAME aeg_reflection_smoke COMMAND aeg_reflection_smoke)
    add_test(NAME aeg_serialization_smoke COMMAND aeg_serialization_smoke)
    add_test(NAME aeg_concurrency_smoke COMMAND aeg_concurrency_smoke)
    add_test(NAME aeg_ecs_smoke COMMAND aeg_ecs_smoke)
    add_test(NAME aeg_pipeline_smoke COMMAND aeg_pipeline_smoke)
    add_test(NAME aeg_pipeline_extension_smoke COMMAND aeg_pipeline_extension_smoke)
    add_test(NAME aeg_pipeline_conformance_smoke COMMAND aeg_pipeline_conformance_smoke)
    add_test(NAME aeg_pipeline_runtime_smoke COMMAND aeg_pipeline_runtime_smoke)
    add_test(NAME aeg_pipeline_hot_swap_smoke COMMAND aeg_pipeline_hot_swap_smoke)
    add_test(NAME aeg_pipeline_phase_scheduler_smoke COMMAND aeg_pipeline_phase_scheduler_smoke)
    add_test(NAME aeg_module_smoke COMMAND aeg_module_smoke)
    add_test(NAME aeg_world_scope_smoke COMMAND aeg_world_scope_smoke)
    add_test(NAME aeg_world_serialization_smoke COMMAND aeg_world_serialization_smoke)
    add_test(NAME aeg_scene_asset_smoke COMMAND aeg_scene_asset_smoke)
    add_test(NAME aeg_world_frame_orchestrator_smoke COMMAND aeg_world_frame_orchestrator_smoke)
    add_test(NAME aeg_world_multi_view_smoke COMMAND aeg_world_multi_view_smoke)
    add_test(NAME aeg_engine_frame_orchestrator_smoke COMMAND aeg_engine_frame_orchestrator_smoke)
    add_test(NAME aeg_renderer_smoke COMMAND aeg_renderer_smoke)
    add_test(NAME aeg_render_feature_runtime_smoke COMMAND aeg_render_feature_runtime_smoke)
    add_test(NAME aeg_frame_retention_smoke COMMAND aeg_frame_retention_smoke)
    add_test(NAME aeg_rhi_smoke COMMAND aeg_rhi_smoke)
    add_test(NAME aeg_logging_smoke COMMAND aeg_logging_smoke)
    add_test(NAME aeg_triangle COMMAND aeg_triangle --frames 1)
    if (WIN32)
        add_test(NAME aeg_editor_imgui_renderer_smoke COMMAND aeg_editor_imgui_renderer_smoke)
        add_test(NAME aeg_editor_application COMMAND aeg_editor_application --frames 3)
    endif()
    if (MINGW)
        get_filename_component(AEG_MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_tests_properties(aeg_application_platform_win32_smoke aeg_editor_imgui_smoke aeg_editor_imgui_renderer_smoke aeg_math_smoke aeg_result_smoke aeg_reflection_smoke aeg_serialization_smoke
                aeg_concurrency_smoke aeg_ecs_smoke aeg_pipeline_smoke aeg_pipeline_extension_smoke
                aeg_pipeline_conformance_smoke aeg_pipeline_runtime_smoke aeg_pipeline_hot_swap_smoke
                aeg_pipeline_phase_scheduler_smoke aeg_module_smoke aeg_world_scope_smoke aeg_world_serialization_smoke
                aeg_scene_asset_smoke aeg_world_frame_orchestrator_smoke aeg_world_multi_view_smoke
                aeg_engine_frame_orchestrator_smoke aeg_renderer_smoke aeg_render_feature_runtime_smoke
                aeg_frame_retention_smoke aeg_rhi_smoke aeg_logging_smoke aeg_triangle aeg_editor_application PROPERTIES
            ENVIRONMENT "PATH=${AEG_MINGW_RUNTIME_DIR};$ENV{PATH}"
        )
    endif()
endif()
