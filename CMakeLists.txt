cmake_minimum_required(VERSION 3.24)

project(StormModding LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(StormModLoader SHARED
    src/StormModLoader.cpp
)
target_include_directories(StormModLoader PRIVATE include)
set_target_properties(StormModLoader PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_executable(StormModLauncher
    src/StormModLauncher.cpp
)
set_target_properties(StormModLauncher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
)

add_library(D3D9Proxy SHARED
    src/D3D9Proxy.cpp
    src/D3D9Proxy.def
)
set_target_properties(D3D9Proxy PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_library(ExpandedSettings SHARED
    Mods/ExpandedSettings/ExpandedSettings.cpp
)
target_include_directories(ExpandedSettings PRIVATE include)
target_link_libraries(ExpandedSettings PRIVATE d3d9 gdi32)
set_target_properties(ExpandedSettings PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/Mods/ExpandedSettings"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Mods/ExpandedSettings"
)

add_library(PerformanceOptimizer SHARED
    Mods/PerformanceOptimizer/PerformanceOptimizer.cpp
)
target_include_directories(PerformanceOptimizer PRIVATE include)
target_link_libraries(PerformanceOptimizer PRIVATE d3d9 dxgi)
set_target_properties(PerformanceOptimizer PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/Mods/PerformanceOptimizer"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Mods/PerformanceOptimizer"
)

add_library(GraphicsEnhancer SHARED
    Mods/GraphicsEnhancer/GraphicsEnhancer.cpp
    Mods/GraphicsEnhancer/ModernLighting.cpp
)
target_include_directories(GraphicsEnhancer PRIVATE include)
target_link_libraries(GraphicsEnhancer PRIVATE d3d9)
set_target_properties(GraphicsEnhancer PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/Mods/GraphicsEnhancer"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Mods/GraphicsEnhancer"
)

if(MSVC)
    foreach(target StormModLoader StormModLauncher D3D9Proxy ExpandedSettings PerformanceOptimizer GraphicsEnhancer)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endforeach()
endif()
