cmake_minimum_required(VERSION 3.30)
project(jakki VERSION 0.2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(OpenSSL REQUIRED)
find_package(Opus REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)
if(UNIX)
    find_package(libpipewire-0.3 REQUIRED)
    find_package(libnotify REQUIRED)
endif()

qt_standard_project_setup()
qt_add_executable(jakki
    src/main.cpp
    src/gui.hpp
    src/gui.cpp
    src/network.hpp
    src/network.cpp
    src/audio.hpp
    src/audio.cpp
)
qt_add_ui(jakki SOURCES ui/main.ui ui/connect.ui ui/textchannel.ui ui/settings.ui)

target_link_libraries(jakki PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets OpenSSL::SSL OpenSSL::Crypto Opus::opus nlohmann_json::nlohmann_json avcodec avformat Threads::Threads)

if(WIN32)
    # target_link_libraries(jakki PRIVATE wsock32 ws2_32 ole32 dwmapi)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD COMMAND Qt6::windeployqt --qmldir "${CMAKE_CURRENT_SOURCE_DIR}" "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" ) 
elseif(UNIX)
    target_link_libraries(jakki PRIVATE libpipewire)
endif()
