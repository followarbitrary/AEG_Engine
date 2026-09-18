cmake_minimum_required(VERSION 3.25)
project(MyGameProject LANGUAGES CXX)
set(AEG_BUILD_TOOLS ON CACHE BOOL "Build metadata and cook tools for the game Editor" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")
set(GAME_BUILD_ID "dev")
set(GAME_SOURCES
    Source/GameModule.cpp
    Source/Render/Pipelines/CustomForwardPipeline.cpp
    Source/Render/Pipelines/CustomForwardPipeline.hpp
    Source/Render/Features/CustomOutlineFeature.cpp
    Source/Render/Features/CustomOutlineFeature.hpp
    Source/Render/Passes/VolumetricFogPass.cpp
    Source/Render/Passes/VolumetricFogPass.hpp
)
aeg_add_game_module(GameModule
    SOURCES ${GAME_SOURCES}
    METADATA "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameModule.metadata.json"
    BUILD_ID "${GAME_BUILD_ID}"
    TARGET_PLATFORM "windows-x64-vulkan"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/Editor/windows-x64-vulkan/${GAME_BUILD_ID}/$<CONFIG>")
aeg_add_game_executable(MyGameExecutable
    MODULE GameModule
    BUILD_ID "${GAME_BUILD_ID}"
    TARGET_PLATFORM "windows-x64-vulkan"
    APP_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameApp.cpp")
