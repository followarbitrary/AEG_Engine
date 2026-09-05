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

add_executable(aeg_result_smoke
    tests/result_smoke.cpp
)
target_link_libraries(aeg_result_smoke PRIVATE aeg_foundation)
if (WIN32)
    target_compile_definitions(aeg_result_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(aeg_pipeline STATIC
    Engine/Pipeline/private/pipeline.cpp
)
target_compile_features(aeg_pipeline PUBLIC cxx_std_20)
target_include_directories(aeg_pipeline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/Pipeline/public
)
target_link_libraries(aeg_pipeline PUBLIC aeg_foundation Threads::Threads)

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

add_executable(aeg_pipeline_runtime_smoke
    tests/pipeline_runtime_smoke.cpp
)
target_link_libraries(aeg_pipeline_runtime_smoke PRIVATE aeg_pipeline)
if (WIN32)
    target_compile_definitions(aeg_pipeline_runtime_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
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
)
target_compile_features(aeg_render_graph PUBLIC cxx_std_20)
target_include_directories(aeg_render_graph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RenderGraph/public
)
target_link_libraries(aeg_render_graph PUBLIC aeg_foundation aeg_rhi_core)

if (MSVC)
    target_compile_options(aeg_render_graph PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_render_graph PRIVATE -Wall -Wextra -Wpedantic)
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
    add_test(NAME aeg_pipeline_smoke COMMAND aeg_pipeline_smoke)
    add_test(NAME aeg_pipeline_extension_smoke COMMAND aeg_pipeline_extension_smoke)
    add_test(NAME aeg_pipeline_runtime_smoke COMMAND aeg_pipeline_runtime_smoke)
    add_test(NAME aeg_rhi_smoke COMMAND aeg_rhi_smoke)
    add_test(NAME aeg_logging_smoke COMMAND aeg_logging_smoke)
    add_test(NAME aeg_triangle COMMAND aeg_triangle --frames 1)
    if (MINGW)
        get_filename_component(AEG_MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_tests_properties(aeg_math_smoke aeg_result_smoke aeg_pipeline_smoke aeg_pipeline_extension_smoke aeg_pipeline_runtime_smoke aeg_rhi_smoke aeg_logging_smoke aeg_triangle PROPERTIES
            ENVIRONMENT "PATH=${AEG_MINGW_RUNTIME_DIR};$ENV{PATH}"
        )
    endif()
endif()
