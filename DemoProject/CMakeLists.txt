cmake_minimum_required(VERSION 3.25)
project(AEGDemo LANGUAGES CXX)

set(AEG_BUILD_TOOLS ON CACHE BOOL "Build metadata and cook tools for the game Editor" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")

set(AEG_DEMO_BUILD_ID "dev")
aeg_add_game_module(GameModule
    SOURCES Source/GameModule.cpp
    METADATA "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameModule.metadata.json"
    BUILD_ID "${AEG_DEMO_BUILD_ID}"
    TARGET_PLATFORM "windows-x64-vulkan"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/Editor/windows-x64-vulkan/${AEG_DEMO_BUILD_ID}/$<CONFIG>")
aeg_add_game_executable(AEGDemoExecutable
    MODULE GameModule
    BUILD_ID "${AEG_DEMO_BUILD_ID}"
    TARGET_PLATFORM "windows-x64-vulkan"
    APP_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameApp.cpp")
