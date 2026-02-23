cmake_minimum_required(VERSION 3.30)
project(jakki VERSION 0.2.0 LANGUAGES CXX HOMEPAGE_URL https://github.com/Jan5u/jakki)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Qt6Keychain REQUIRED)
if(UNIX)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    pkg_check_modules(LIBNOTIFY REQUIRED libnotify)
    pkg_check_modules(AVFORMAT REQUIRED libavformat)
    pkg_check_modules(AVCODEC REQUIRED libavcodec)
    pkg_check_modules(AVUTIL REQUIRED libavutil)
    pkg_check_modules(OPUS REQUIRED opus)
    pkg_check_modules(GIO REQUIRED gio-2.0)
    pkg_check_modules(ZSTD REQUIRED libzstd)
elseif(WIN32)
    find_package(FFMPEG REQUIRED)
    find_package(Opus REQUIRED)
    find_package(zstd CONFIG REQUIRED)
endif()

include_directories(${PIPEWIRE_INCLUDE_DIRS})

# Find glslc for compiling shaders
find_program(GLSLC glslc)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found. Please install the Vulkan SDK.")
endif()

# Function to compile GLSL shaders to SPIR-V
function(compile_shader target shader_file)
    get_filename_component(shader_name ${shader_file} NAME_WE)
    get_filename_component(shader_ext ${shader_file} EXT)
    string(SUBSTRING ${shader_ext} 1 -1 ext_without_dot)
    set(output_file "${CMAKE_CURRENT_SOURCE_DIR}/ui/shaders/${shader_name}_${ext_without_dot}.spv")
    add_custom_command(
        OUTPUT ${output_file}
        COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/${shader_file} -o ${output_file}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader_file}
        COMMENT "Compiling shader: ${shader_file} -> ${shader_name}_${ext_without_dot}.spv"
    )
    target_sources(${target} PRIVATE ${output_file})
endfunction()

qt_standard_project_setup()
set(COMMON_SOURCES
    src/main.cpp
    src/gui.hpp
    src/gui.cpp
    src/network.hpp
    src/network.cpp
    src/auth.hpp
    src/auth.cpp
    src/audio/audio_impl.hpp
    src/audio/audio_impl.cpp
    src/audio/audio.hpp
    src/audio/audio.cpp
    src/config.hpp
    src/config.cpp
    src/chat/text.hpp
    src/chat/text.cpp
    src/chat/emoji_data.hpp
    src/chat/emoji_data.cpp
    src/chat/emote_manager.hpp
    src/chat/emote_manager.cpp
    src/chat/emote_completer.hpp
    src/chat/emote_completer.cpp
    src/video/video.hpp
    src/video/video.cpp
    src/video/decode/decoder.hpp
    src/video/decode/decoder.cpp
    src/video/render/vulkan_renderer.hpp
    src/video/render/vulkan_renderer.cpp
    src/video/render/screen_renderer.hpp
    src/video/render/screen_renderer.cpp
    src/video/encode/encoder.hpp
    src/video/encode/encoder.cpp
    src/video/capture/capture.hpp
    src/video/capture/capture.cpp
)
if(WIN32)
    list(APPEND COMMON_SOURCES
        src/audio/wasapi_impl.hpp
        src/audio/wasapi_impl.cpp
        src/video/capture/capture_windows.hpp
        src/video/capture/capture_windows.cpp
        src/video/encode/nvenc_windows.hpp
        src/video/encode/nvenc_windows.cpp
        ui/jakki.rc
    )
else()
    list(APPEND COMMON_SOURCES
        src/audio/pipewire_impl.hpp
        src/audio/pipewire_impl.cpp
        src/video/capture/capture_linux.hpp
        src/video/capture/capture_linux.cpp
        src/video/encode/nvenc_linux.hpp
        src/video/encode/nvenc_linux.cpp
    )
endif()

qt_add_executable(jakki ui/jakki.qrc ${COMMON_SOURCES})

qt_add_ui(jakki SOURCES ui/main.ui ui/connect.ui ui/textchannel.ui ui/settings.ui ui/adminpanel.ui ui/screenshare.ui)

compile_shader(jakki ui/shaders/screen.vert)
compile_shader(jakki ui/shaders/screen.frag)
compile_shader(jakki ui/shaders/bgra_to_nv12.comp)

target_link_libraries(jakki PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6Keychain::Qt6Keychain OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json Threads::Threads)

if(WIN32)
    set_target_properties(jakki PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
    target_link_libraries(jakki PRIVATE Opus::opus ${FFMPEG_LIBRARIES} zstd::libzstd)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD COMMAND Qt6::windeployqt --qmldir "${CMAKE_CURRENT_SOURCE_DIR}" "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" ) 
elseif(UNIX)
    target_link_libraries(jakki PRIVATE ${OPUS_LIBRARIES} ${PIPEWIRE_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${LIBNOTIFY_LIBRARIES} ${GIO_LIBRARIES} ${ZSTD_LIBRARIES})
    target_include_directories(jakki PRIVATE ${GIO_INCLUDE_DIRS})
endif()

install(TARGETS jakki
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
    install(CODE [[
        file(GLOB DLL_FILES "$<TARGET_FILE_DIR:jakki>/*.dll")
        file(INSTALL ${DLL_FILES} DESTINATION "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/bin")
    ]])
endif()

qt_generate_deploy_app_script(
    TARGET jakki
    OUTPUT_SCRIPT deploy_script
)
install(SCRIPT ${deploy_script})

set(CPACK_PACKAGE_NAME "Jakki")
set(CPACK_PACKAGE_VENDOR "Jakki")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Jakki")
set(CPACK_PACKAGE_VERSION_MAJOR ${CMAKE_PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${CMAKE_PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${CMAKE_PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/package")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Jakki")
set(CPACK_PACKAGE_EXECUTABLES "Jakki" "Jakki")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/ui/images/icon.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/ui/images/icon.ico")
SET(CPACK_NSIS_INSTALLED_ICON_NAME "bin/jakki.exe")
SET(CPACK_NSIS_MUI_FINISHPAGE_RUN "jakki.exe")

include(CPack)
