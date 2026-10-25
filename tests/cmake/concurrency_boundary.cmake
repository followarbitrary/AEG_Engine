# Engine-internal execution facilities belong to Engine/Concurrency. This
# policy does not constrain game-authored sources; those sources are checked
# separately for accidental references to the internal API.
# Win32Window::focus keeps GetCurrentThreadId/GetWindowThreadProcessId because
# AttachThreadInput requires native Win32 thread IDs; this does not create a thread.
if(NOT DEFINED AEG_SOURCE_DIR)
    message(FATAL_ERROR "AEG_SOURCE_DIR is required")
endif()

set(violations "")
foreach(root Engine Application tests tools)
    file(GLOB_RECURSE sources
        "${AEG_SOURCE_DIR}/${root}/*.hpp" "${AEG_SOURCE_DIR}/${root}/*.h"
        "${AEG_SOURCE_DIR}/${root}/*.cpp" "${AEG_SOURCE_DIR}/${root}/*.cc"
        "${AEG_SOURCE_DIR}/${root}/*.cxx")
    foreach(source IN LISTS sources)
        file(RELATIVE_PATH relative "${AEG_SOURCE_DIR}" "${source}")
        if(relative MATCHES "^Engine/Concurrency/" OR relative MATCHES "^Application/Project/templates/")
            continue()
        endif()
        file(READ "${source}" contents)
        if(contents MATCHES "#[ \t]*include[ \t]*[<\\\"](thread|future|coroutine|stop_token)[>\\\"]" OR
           contents MATCHES "std::(thread|jthread|this_thread|async|future|promise|packaged_task|stop_token|stop_source|stop_callback|coroutine_handle|suspend_always|suspend_never)" OR
           contents MATCHES "native_stop_token[ \\t]*\\(" OR
           contents MATCHES "\\.stop_token([ \\t\\r\\n]|$)" OR
           contents MATCHES "(CreateThread|_beginthreadex|pthread_create|CreateFiber|SwitchToFiber|usleep)[ \t]*\\(")
            list(APPEND violations "${relative}")
        endif()
    endforeach()
endforeach()
if(violations)
    list(JOIN violations "\n  " details)
    message(FATAL_ERROR "Use aeg::concurrency for first-party threads/coroutines:\n  ${details}")
endif()

set(game_surface_violations "")
foreach(root Application/Project/templates/game/Source DemoProject/Source)
    file(GLOB_RECURSE sources
        "${AEG_SOURCE_DIR}/${root}/*.hpp" "${AEG_SOURCE_DIR}/${root}/*.h"
        "${AEG_SOURCE_DIR}/${root}/*.cpp" "${AEG_SOURCE_DIR}/${root}/*.cc"
        "${AEG_SOURCE_DIR}/${root}/*.cxx")
    foreach(source IN LISTS sources)
        file(RELATIVE_PATH relative "${AEG_SOURCE_DIR}" "${source}")
        file(READ "${source}" contents)
        if(contents MATCHES "#[ \t]*include[ \t]*[<\\\"]concurrency[.]hpp[>\\\"]" OR
           contents MATCHES "aeg::concurrency([:; \t\r\n]|$)")
            list(APPEND game_surface_violations "${relative}")
        endif()
    endforeach()
endforeach()
if(game_surface_violations)
    list(JOIN game_surface_violations "\n  " details)
    message(FATAL_ERROR "Game-authored sources must not depend on Engine/Concurrency:\n  ${details}")
endif()
message(STATUS "Engine-internal Concurrency boundary passed; game surface is clean")
