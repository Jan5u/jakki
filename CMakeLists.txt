cmake_minimum_required(VERSION 3.30)
project(jakki VERSION 0.2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json REQUIRED)
if(UNIX)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    pkg_check_modules(LIBNOTIFY REQUIRED libnotify)
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(OPUS REQUIRED opus)
elseif(WIN32)
    find_package(FFMPEG REQUIRED)
    find_package(Opus REQUIRED)
endif()

include_directories(${PIPEWIRE_INCLUDE_DIRS})

qt_standard_project_setup()
set(COMMON_SOURCES
    src/main.cpp
    src/gui.hpp
    src/gui.cpp
    src/network.hpp
    src/network.cpp
    src/audio/audio_impl.hpp
    src/audio/audio_impl.cpp
    src/audio/audio.hpp
    src/audio/audio.cpp
    src/config.hpp
    src/config.cpp
)
if(WIN32)
    list(APPEND COMMON_SOURCES
        src/audio/wasapi_impl.hpp
        src/audio/wasapi_impl.cpp
        ui/jakki.rc
    )
else()
    list(APPEND COMMON_SOURCES
        src/audio/pipewire_impl.hpp
        src/audio/pipewire_impl.cpp
    )
endif()

qt_add_executable(jakki ui/jakki.qrc ${COMMON_SOURCES})
qt_add_ui(jakki SOURCES ui/main.ui ui/connect.ui ui/textchannel.ui ui/settings.ui)

target_link_libraries(jakki PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json Threads::Threads)

if(WIN32)
    target_link_libraries(jakki PRIVATE Opus::opus ${FFMPEG_LIBRARIES})
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD COMMAND Qt6::windeployqt --qmldir "${CMAKE_CURRENT_SOURCE_DIR}" "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" ) 
elseif(UNIX)
    target_link_libraries(jakki PRIVATE ${OPUS_LIBRARIES} ${PIPEWIRE_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${LIBNOTIFY_LIBRARIES})
endif()
