cmake_minimum_required(VERSION 3.20)
project(PhoneLinkLock LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project only builds on Windows (uses Win32 API + UI Automation).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE)

# Build as a GUI (WIN32) subsystem app -> no console window, runs quietly like a background service.
add_executable(PhoneLinkLock WIN32
    src/main.cpp
)

target_link_libraries(PhoneLinkLock PRIVATE
    user32
    shell32
    ole32
    oleaut32
    uiautomationcore
    wtsapi32
    userenv
)

# Static-link the CRT so the .exe runs on machines without the VC++ redistributable installed.
if(MSVC)
    set_property(TARGET PhoneLinkLock PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS PhoneLinkLock RUNTIME DESTINATION .)
