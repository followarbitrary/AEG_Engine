cmake_minimum_required(VERSION 3.25)
project(AegContractExporterFixture LANGUAGES CXX)

if(NOT DEFINED ENGINE_DIR)
    message(FATAL_ERROR "Contract exporter fixture requires ENGINE_DIR")
endif()
set(AEG_BUILD_EDITOR OFF CACHE BOOL "" FORCE)
set(AEG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AEG_BUILD_TOOLS ON CACHE BOOL "" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")

# The DLL and host exporter must compile the same factory and helper sources.
aeg_add_game_module(ContractExporterFixture
    SOURCES Source/ModuleFactory.cpp Source/FeatureDefinition.cpp Source/ModuleEntry.cpp
    METADATA_FACTORY_HEADER "Source/ModuleFactory.hpp"
    METADATA_FACTORY "exporter_fixture::make_module"
    BUILD_ID "contract-exporter-fixture"
    TARGET_PLATFORM "windows-x64-vulkan"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ModuleBundle/$<CONFIG>")

add_executable(ContractExporterFixtureVerifier Source/VerifyBundle.cpp)
target_compile_features(ContractExporterFixtureVerifier PRIVATE cxx_std_20)
target_link_libraries(ContractExporterFixtureVerifier PRIVATE aeg_module aeg_feature)
target_include_directories(ContractExporterFixtureVerifier PRIVATE
    "${ENGINE_DIR}/Engine/Feature/private")

if(NOT TARGET ContractExporterFixture_contract_exporter OR
   NOT TARGET ContractExporterFixture_contract_metadata)
    message(FATAL_ERROR "FACTORY_EXPORTER_MISSING: aeg_add_game_module did not create its host exporter/metadata targets")
endif()
