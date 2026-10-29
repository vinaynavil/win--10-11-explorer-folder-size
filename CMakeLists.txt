cmake_minimum_required(VERSION 3.20)
project(ExplorerFolderSize VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(ExplorerFolderSize WIN32
    src/main.cpp
    src/ExplorerTracker.h
    src/ExplorerTracker.cpp
    src/SizeCalculator.h
    src/SizeCalculator.cpp
    src/OverlayRenderer.h
    src/OverlayRenderer.cpp
)

if(MSVC)
    target_compile_options(ExplorerFolderSize PRIVATE /W4 /MP /utf-8 /permissive-)
    set_property(TARGET ExplorerFolderSize PROPERTY VS_DPI_AWARE "PerMonitor")
endif()

target_link_libraries(ExplorerFolderSize PRIVATE
    user32
    gdi32
    shell32
    ole32
    oleaut32
    uuid
    d2d1
    dwrite
    dwmapi
    shlwapi
    advapi32
)
