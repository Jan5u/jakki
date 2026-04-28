cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(jakki LANGUAGES CXX)

find_package(SDL3 REQUIRED)
find_package(imgui)

include(FetchContent)

if(NOT imgui_FOUND)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.7-docking
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp

        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
    )

    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    
    add_library(imgui::imgui ALIAS imgui)
endif()



add_library(platform
  src/platform/window.hpp
  src/platform/window.cpp
)

target_include_directories(platform
PUBLIC
  src
)

add_executable(jakki src/main.cpp)

target_link_libraries(platform PUBLIC
  SDL3::SDL3
  imgui::imgui
)

target_link_libraries(jakki PRIVATE platform)
