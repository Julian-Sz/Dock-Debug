cmake_minimum_required(VERSION 3.20)
project(DockDebug LANGUAGES CXX)

add_executable(dock-debug WIN32
    src/main.cpp
    src/monitor.cpp
    src/monitor.h
    src/report.cpp
    src/report.h
    src/dock-debug.manifest
)

add_executable(dock-debug-service WIN32
    src/service_main.cpp
    src/monitor.cpp
    src/monitor.h
    src/dock-debug.manifest
)

target_compile_features(dock-debug PRIVATE cxx_std_17)
target_compile_definitions(dock-debug PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dock-debug PRIVATE setupapi cfgmgr32 user32 gdi32 comctl32)
target_link_libraries(dock-debug-service PRIVATE setupapi cfgmgr32 user32 advapi32 comctl32)

if(MSVC)
    target_compile_options(dock-debug PRIVATE /W4 /permissive-)
    target_compile_options(dock-debug-service PRIVATE /W4 /permissive-)
else()
    target_compile_options(dock-debug PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(dock-debug-service PRIVATE -Wall -Wextra -Wpedantic)
endif()
