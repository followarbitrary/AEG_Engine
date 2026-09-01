cmake_minimum_required(VERSION 3.25)
project(AEG_Engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AEG_RHI_ENABLE_VALIDATION "Compile Vulkan validation-layer support" ON)
option(AEG_RHI_ENABLE_DEBUG_UTILS "Compile Vulkan debug-utils support" ${AEG_RHI_ENABLE_VALIDATION})
option(AEG_RHI_ENABLE_DEBUG_NAMES "Compile Vulkan object debug-name support" ${AEG_RHI_ENABLE_DEBUG_UTILS})

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

if (AEG_RHI_ENABLE_DEBUG_NAMES AND NOT AEG_RHI_ENABLE_DEBUG_UTILS)
    message(FATAL_ERROR
        "AEG_RHI_ENABLE_DEBUG_NAMES requires AEG_RHI_ENABLE_DEBUG_UTILS=ON")
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

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

set(AEG_VMA_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/VulkanMemoryAllocator/include")
if (NOT EXISTS "${AEG_VMA_INCLUDE_DIR}/vk_mem_alloc.h")
    message(FATAL_ERROR
        "Vulkan Memory Allocator source is missing: "
        "${AEG_VMA_INCLUDE_DIR}/vk_mem_alloc.h")
endif()

add_library(aeg_rhi STATIC
        Engine/RHI/private/vulkan/vulkan_device.cpp
        Engine/RHI/private/vulkan/vma_impl.cpp
)

target_compile_features(aeg_rhi PUBLIC cxx_std_20)
target_compile_definitions(aeg_rhi
    PUBLIC
        AEG_RHI_ENABLE_VALIDATION=$<BOOL:${AEG_RHI_ENABLE_VALIDATION}>
        AEG_RHI_ENABLE_DEBUG_UTILS=$<BOOL:${AEG_RHI_ENABLE_DEBUG_UTILS}>
        AEG_RHI_ENABLE_DEBUG_NAMES=$<BOOL:${AEG_RHI_ENABLE_DEBUG_NAMES}>
    PRIVATE
        VMA_VULKAN_VERSION=1003000
)
if (WIN32)
    target_compile_definitions(aeg_rhi PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
endif()
target_include_directories(aeg_rhi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine/RHI/public
)
target_include_directories(aeg_rhi SYSTEM PRIVATE ${AEG_VMA_INCLUDE_DIR})
target_link_libraries(aeg_rhi PUBLIC aeg_logging Vulkan::Vulkan)

if (MSVC)
    target_compile_options(aeg_rhi PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeg_rhi PRIVATE -Wall -Wextra -Wpedantic)
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

include(CTest)
if (BUILD_TESTING)
    add_test(NAME aeg_rhi_smoke COMMAND aeg_rhi_smoke)
    add_test(NAME aeg_logging_smoke COMMAND aeg_logging_smoke)
    if (MINGW)
        get_filename_component(AEG_MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_tests_properties(aeg_rhi_smoke aeg_logging_smoke PROPERTIES
            ENVIRONMENT "PATH=${AEG_MINGW_RUNTIME_DIR};$ENV{PATH}"
        )
    endif()
endif()
