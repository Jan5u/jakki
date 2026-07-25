cmake_minimum_required(VERSION 4.2)
project(jakki VERSION 0.2.0 LANGUAGES CXX HOMEPAGE_URL https://github.com/Jan5u/jakki)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

if(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
endif()

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

set(COMMON_SOURCES
    src/main.cpp
    src/network.hpp
    src/network.cpp
    src/auth.hpp
    src/auth.cpp
    src/gui.hpp
    src/gui.cpp
    src/audio/audio.hpp
    src/audio/audio.cpp
    src/audio/pipewire_impl.hpp
    src/audio/pipewire_impl.cpp
    src/audio/audio_impl.hpp
    src/audio/audio_impl.cpp
    src/config.hpp
    src/config.cpp
    src/video/video.hpp
    src/video/video.cpp
)

add_executable(jakki
    ${COMMON_SOURCES}
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
    third_party/imgui/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(jakki PRIVATE
    third_party/imgui
    third_party/imgui/backends
    third_party/imgui/misc/cpp
)

compile_shader(jakki ui/shaders/screen.vert)
compile_shader(jakki ui/shaders/screen.frag)
compile_shader(jakki ui/shaders/bgra_to_nv12.comp)


include(ExternalProject)
set(FFMPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/ffmpeg-install)

ExternalProject_Add(ffmpeg
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg
    BUILD_IN_SOURCE 1

    CONFIGURE_COMMAND ./configure
        --prefix=${FFMPEG_INSTALL_DIR}
        --disable-programs
        --disable-doc
        --disable-all
        --disable-autodetect
        --disable-static
        --enable-shared
        --enable-avcodec
        --enable-avformat
        --enable-protocol=file
        --enable-demuxer=mov
        --enable-parser=h264,hevc,av1
        --enable-vulkan
        --enable-hwaccel=av1_vulkan,h264_vulkan,hevc_vulkan
        --enable-encoder=av1_vulkan,h264_vulkan,hevc_vulkan
        
    BUILD_COMMAND make -j
    INSTALL_COMMAND make install
)

target_include_directories(jakki PRIVATE
    ${FFMPEG_INSTALL_DIR}/include
)

target_link_directories(jakki PRIVATE
    ${FFMPEG_INSTALL_DIR}/lib
)

add_dependencies(jakki ffmpeg)



add_subdirectory(third_party/sdl)

target_link_libraries(jakki PRIVATE 
    SDL3::SDL3
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    PkgConfig::LIBSECRET
    PkgConfig::PIPEWIRE
    PkgConfig::OPUS
    Threads::Threads
    avcodec
    avformat
    avutil
)

# if(WIN32)
#     set_target_properties(jakki PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
#     target_link_libraries(jakki PRIVATE Opus::opus ${FFMPEG_LIBRARIES} zstd::libzstd)
#     add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD COMMAND Qt6::windeployqt --qmldir "${CMAKE_CURRENT_SOURCE_DIR}" "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" ) 
# elseif(UNIX)
#     target_link_libraries(jakki PRIVATE ${OPUS_LIBRARIES} ${PIPEWIRE_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${LIBNOTIFY_LIBRARIES} ${GIO_LIBRARIES} ${ZSTD_LIBRARIES})
#     target_include_directories(jakki PRIVATE ${GIO_INCLUDE_DIRS})
# endif()

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
