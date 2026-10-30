cmake_minimum_required(VERSION 3.15)
project(d3d8_patch CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force static runtime library (/MT in Release, /MTd in Debug)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MSVC)
    add_compile_options(/utf-8)
endif()

add_library(d3d8 SHARED
    src/main.cpp
    src/d3d8_proxy.cpp
    src/d3d8.def
)

target_link_libraries(d3d8 PRIVATE winmm)

set_target_properties(d3d8 PROPERTIES
    OUTPUT_NAME "d3d8"
    PREFIX ""
    SUFFIX ".dll"
)
