cmake_minimum_required(VERSION 3.25)
project(AegModulePublicationFixture LANGUAGES CXX)

if(NOT DEFINED ENGINE_DIR)
    message(FATAL_ERROR "Module publication fixture requires ENGINE_DIR")
endif()
set(AEG_BUILD_EDITOR OFF CACHE BOOL "" FORCE)
set(AEG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AEG_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(PUBLICATION_BUILD_ID "publication-v1" CACHE STRING "Metadata identity changed without editing DLL sources")
include("${ENGINE_DIR}/cmake/EngineTools.cmake")

# The driver copies the existing exporter's real factory sources into Source/.
# Both branches continue to compile that same factory; no JSON is a contract source.
aeg_add_game_module(PublicationFixture
    SOURCES Source/ModuleFactory.cpp Source/FeatureDefinition.cpp Source/ModuleEntry.cpp
    METADATA_FACTORY_HEADER "Source/ModuleFactory.hpp"
    METADATA_FACTORY "exporter_fixture::make_module"
    BUILD_ID "${PUBLICATION_BUILD_ID}"
    TARGET_PLATFORM "windows-x64-vulkan"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ModuleBundle/$<CONFIG>")

# Keep the existing explicit-METADATA form covered as well. The driver writes
# this file from the real factory's export before building this target.
aeg_add_game_module(PublicationMetadataOnlyFixture
    SOURCES Source/ModuleFactory.cpp Source/FeatureDefinition.cpp Source/ModuleEntry.cpp
    METADATA "${CMAKE_CURRENT_SOURCE_DIR}/OnlyMetadata.json"
    BUILD_ID "publication-v1"
    TARGET_PLATFORM "windows-x64-vulkan"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/MetadataOnlyBundle/$<CONFIG>")

add_executable(PublicationVerifier VerifyPublication.cpp)
target_compile_features(PublicationVerifier PRIVATE cxx_std_20)
target_link_libraries(PublicationVerifier PRIVATE aeg_module aeg_feature)
target_include_directories(PublicationVerifier PRIVATE "${ENGINE_DIR}/Engine/Feature/private")
if(MSVC)
    target_compile_options(PublicationVerifier PRIVATE /utf-8)
endif()
