cmake_minimum_required(VERSION 3.25)

if (NOT DEFINED ENGINE_DIR OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ENGINE_DIR and TEST_ROOT are required")
endif()

# Reuse the external-project setup and add the full publication and negative
# fixture checks required for the runtime release gate.
set(AEG_GAME_PROJECT_RUNTIME ON)
include("${CMAKE_CURRENT_LIST_DIR}/extensibility_external_project.cmake")
