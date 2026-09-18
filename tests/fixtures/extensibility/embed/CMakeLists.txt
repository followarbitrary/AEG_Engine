cmake_minimum_required(VERSION 3.25)
project(AegEmbedFixture LANGUAGES CXX)

set(AEG_EMBED_FIXTURE_GENERATE OFF CACHE BOOL "")
set(AEG_BUILD_EDITOR ${AEG_EMBED_FIXTURE_GENERATE} CACHE BOOL "" FORCE)
set(AEG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AEG_BUILD_TOOLS ${AEG_EMBED_FIXTURE_GENERATE} CACHE BOOL "" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")

if (NOT AEG_EMBED_FIXTURE_GENERATE)
    foreach(forbidden IN ITEMS
            aeg_editor_application
            aeg_imgui_core
            aeg_application_tooling
            aeg_project_smoke)
        if(TARGET "${forbidden}")
            message(FATAL_ERROR "Unexpected target: ${forbidden}")
        endif()
    endforeach()
endif()

add_library(embed_consumer STATIC consumer.cpp)
target_link_libraries(embed_consumer PRIVATE aeg_engine)
