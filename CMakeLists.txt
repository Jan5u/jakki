cmake_minimum_required(VERSION 4.2)
project(jakki VERSION 0.2.0 LANGUAGES CXX HOMEPAGE_URL https://github.com/Jan5u/jakki)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

if(CMAKE_CROSSCOMPILING)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(NLOHMANN_JSON_CROSS_INCLUDE_DIR "${CMAKE_BINARY_DIR}/host-headers")
    file(COPY "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann" DESTINATION "${NLOHMANN_JSON_CROSS_INCLUDE_DIR}")
    add_library(nlohmann_json_cross INTERFACE)
    target_include_directories(nlohmann_json_cross INTERFACE "${NLOHMANN_JSON_CROSS_INCLUDE_DIR}")
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_cross)
else()
    find_package(nlohmann_json REQUIRED)
endif()

if(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
endif()

# Find glslc for compiling shaders
find_program(GLSLC glslc)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found. Please install the Vulkan SDK.")
endif()

function(compile_shader target shader_file)
    get_filename_component(shader_name ${shader_file} NAME_WE)
    get_filename_component(shader_ext ${shader_file} EXT)
    string(SUBSTRING ${shader_ext} 1 -1 shader_stage)
    set(output_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${output_dir})
    set(output_file ${output_dir}/${shader_name}.${shader_stage}.spv)
    add_custom_command(
        OUTPUT ${output_file}
        COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/${shader_file} -o ${output_file}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader_file}
        COMMENT "Compiling ${shader_file}"
        VERBATIM
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
    src/audio/audio_impl.hpp
    src/audio/audio_impl.cpp
    src/config.hpp
    src/config.cpp
    src/video/video.hpp
    src/video/video.cpp
    src/video/encode/encoder.hpp
    src/video/encode/encoder.cpp
    src/video/capture/capture.hpp
    src/video/capture/capture.cpp
)

if(WIN32)
    list(APPEND COMMON_SOURCES
        src/audio/wasapi_impl.hpp
        src/audio/wasapi_impl.cpp
        ui/jakki.rc
    )
else()
    list(APPEND COMMON_SOURCES
        src/portal.hpp
        src/portal.cpp
        src/audio/pipewire_impl.hpp
        src/audio/pipewire_impl.cpp
        src/video/capture/capture_linux.hpp
        src/video/capture/capture_linux.cpp
        src/video/encode/encoder_vulkan.hpp
        src/video/encode/encoder_vulkan.cpp
    )
endif()

add_executable(jakki
    ${COMMON_SOURCES}
)

compile_shader(jakki ui/shaders/bgra_to_nv12.comp)


include(ExternalProject)
set(FFMPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/ffmpeg-install)
set(FFMPEG_INCLUDE_DIR ${FFMPEG_INSTALL_DIR}/include)
set(FFMPEG_LIBRARY_DIR ${FFMPEG_INSTALL_DIR}/lib)

set(FFMPEG_HWACCELS av1_vulkan,h264_vulkan,hevc_vulkan)
set(FFMPEG_ENCODERS av1_vulkan,h264_vulkan,hevc_vulkan)
set(FFMPEG_D3D12VA_OPTIONS)
set(FFMPEG_CROSS_OPTIONS)
if(WIN32)
    set(FFMPEG_D3D12VA_OPTIONS --enable-d3d12va)
    string(APPEND FFMPEG_HWACCELS ,av1_d3d12va,h264_d3d12va,hevc_d3d12va)
    string(APPEND FFMPEG_ENCODERS ,av1_d3d12va,h264_d3d12va,hevc_d3d12va)
endif()
if(MINGW)
    set(FFMPEG_CROSS_OPTIONS
        --enable-cross-compile
        --arch=x86_64
        --target-os=mingw32
        --cross-prefix=x86_64-w64-mingw32-
        --extra-cflags=-I${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
    )
endif()

ExternalProject_Add(ffmpeg
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg
    BUILD_IN_SOURCE 0

    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/config.h
        COMMAND ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/configure
        --prefix=${FFMPEG_INSTALL_DIR}
        ${FFMPEG_CROSS_OPTIONS}
        --disable-programs
        --disable-doc
        --disable-all
        --disable-autodetect
        --disable-static
        --enable-shared
        --enable-avcodec
        --enable-avformat
        --enable-parser=h264,hevc,av1
        --enable-vulkan
        ${FFMPEG_D3D12VA_OPTIONS}
        --enable-hwaccel=${FFMPEG_HWACCELS}
        --enable-encoder=${FFMPEG_ENCODERS}
        
    BUILD_COMMAND make -j
    INSTALL_COMMAND make install
)

target_include_directories(jakki PRIVATE
    ${FFMPEG_INSTALL_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
)

target_link_directories(jakki PRIVATE
    ${FFMPEG_INSTALL_DIR}/lib
)

add_dependencies(jakki ffmpeg)

if(UNIX)
    set(LIBPORTAL_INSTALL_DIR ${CMAKE_BINARY_DIR}/libportal-install)

    ExternalProject_Add(libportal
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/libportal
        BUILD_IN_SOURCE 0

        CONFIGURE_COMMAND meson setup
            <BINARY_DIR>
            <SOURCE_DIR>
            --prefix=${LIBPORTAL_INSTALL_DIR}
            --libdir=lib
            --buildtype=release
            -Dtests=false
            -Dvapi=false
            -Dintrospection=false
            -Ddocs=false
            -Dbackend-gtk3=disabled
            -Dbackend-gtk4=disabled
            -Dbackend-qt5=disabled
            -Dbackend-qt6=disabled

        BUILD_COMMAND meson compile -C <BINARY_DIR>
        INSTALL_COMMAND meson install -C <BINARY_DIR>
    )

    target_include_directories(jakki PRIVATE
        ${LIBPORTAL_INSTALL_DIR}/include
    )

    target_link_directories(jakki PRIVATE
        ${LIBPORTAL_INSTALL_DIR}/lib
    )

    add_dependencies(jakki libportal)
endif()

if(WIN32)
    set(OPENSSL_INSTALL_DIR ${CMAKE_BINARY_DIR}/openssl)

    target_include_directories(jakki PRIVATE
        ${OPENSSL_INSTALL_DIR}/include
    )

    target_link_directories(jakki PRIVATE
        ${OPENSSL_INSTALL_DIR}/lib
        ${OPENSSL_INSTALL_DIR}/lib64
    )

    ExternalProject_Add(openssl
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/openssl
        CONFIGURE_COMMAND
            ./Configure
                mingw64
                --cross-compile-prefix=x86_64-w64-mingw32-
                --prefix=${OPENSSL_INSTALL_DIR}
                no-tests
                no-docs
                shared
                no-legacy
                no-comp
                no-cmp
                no-ct
                no-filenames
                no-http
                no-idea
                no-mdc2
                no-psk
                no-srp
                no-tls1
                no-tls1_1
                no-weak-ssl-ciphers
                no-apps
        BUILD_COMMAND make -j4
        INSTALL_COMMAND make install_sw
        BUILD_IN_SOURCE 1
    )

    add_dependencies(jakki openssl)
endif()

if(WIN32)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/vulkan-headers)

    set(VulkanHeaders_VERSION 1.4.351)
    add_subdirectory(third_party/vulkan-loader)
else()
    find_package(Vulkan REQUIRED)
endif()

set(SDL_GPU OFF)
set(SDL_HAPTIC OFF)
set(SDL_JOYSTICK OFF)
set(SDL_SENSOR OFF)
set(SDL_HIDAPI OFF)
set(SDL_POWER OFF)
set(SDL_SENSOR OFF)

add_subdirectory(third_party/sdl)

set(SDLTTF_VENDORED ON)
set(SDLTTF_HARFBUZZ ON)

add_subdirectory(third_party/sdl_ttf)

set(SDLIMAGE_WEBP OFF)
set(SDLIMAGE_AVIF OFF)

add_subdirectory(third_party/sdl_image)

set(BUILD_SHARED_LIBS ON)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)

add_subdirectory(third_party/opus)

if(WIN32)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/zstd/build/cmake zstd)
endif()

install(TARGETS SDL3-shared
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(TARGETS SDL3_image-shared
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(TARGETS SDL3_ttf-shared
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

target_link_libraries(jakki PRIVATE 
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    nlohmann_json::nlohmann_json
    Opus::opus
    Threads::Threads
    avformat
    avcodec
    avutil
)

if(WIN32)
    if(MINGW)
        target_link_libraries(jakki PRIVATE
            ssl
            crypto
            libzstd_static
            stdc++exp
            Vulkan::Loader
        )
    else()
        target_link_libraries(jakki PRIVATE libssl libcrypto libzstd_static Vulkan::Loader)
    endif()
else()
    target_link_libraries(jakki PRIVATE 
        ssl
        crypto
        portal
        Vulkan::Vulkan
        PkgConfig::LIBSECRET
        PkgConfig::PIPEWIRE
        PkgConfig::ZSTD
    )
endif()

install(CODE [[
    file(REMOVE_RECURSE
        "${CMAKE_INSTALL_PREFIX}/include"
    )
]])

set_target_properties(jakki PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS jakki
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(NOT WIN32)
    install(
        FILES ${CMAKE_SOURCE_DIR}/jakki.desktop
        DESTINATION share/applications
    )

    install(
        FILES ${CMAKE_SOURCE_DIR}/ui/images/icon-256.png
        DESTINATION share/icons/hicolor/256x256/apps
    )

    install(
        DIRECTORY ${CMAKE_SOURCE_DIR}/ui/icons/
        DESTINATION ui/icons
    )

    install(
        DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders/
        DESTINATION share/jakki/shaders
    )

    install(
        DIRECTORY ${FFMPEG_INSTALL_DIR}/lib/
        DESTINATION lib
        FILES_MATCHING PATTERN "*.so*"
    )

    install(
        DIRECTORY ${LIBPORTAL_INSTALL_DIR}/lib/
        DESTINATION lib
        FILES_MATCHING PATTERN "*.so*"
    )
endif()

if(WIN32)
    install(
        DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders/
        DESTINATION shaders
    )

    install(
        DIRECTORY ${CMAKE_SOURCE_DIR}/ui/icons/
        DESTINATION ui/icons
    )

    install(
        DIRECTORY ${FFMPEG_INSTALL_DIR}/bin/
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILES_MATCHING PATTERN "*.dll"
    )

    install(
        DIRECTORY ${OPENSSL_INSTALL_DIR}/bin/
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILES_MATCHING PATTERN "*.dll"
    )

    if(MINGW)
        foreach(runtime_dll libgcc_s_seh-1.dll libssp-0.dll libstdc++-6.dll libwinpthread-1.dll)
            execute_process(
                COMMAND ${CMAKE_CXX_COMPILER} -print-prog-name=${runtime_dll}
                OUTPUT_VARIABLE runtime_dll_path
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(EXISTS "${runtime_dll_path}")
                install(FILES "${runtime_dll_path}" DESTINATION ${CMAKE_INSTALL_BINDIR})
            endif()
        endforeach()
    endif()

    install(CODE [[
        file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/lib")
    ]])
endif()

set(CPACK_PACKAGE_NAME "Jakki")
set(CPACK_PACKAGE_VENDOR "Jakki")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Jakki")
set(CPACK_PACKAGE_VERSION ${CMAKE_PROJECT_VERSION})
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/package")
set(CPACK_PACKAGE_EXECUTABLES "jakki" "Jakki")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_STRIP_FILES OFF)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Jakki")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/ui/images/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/ui/images/icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin/jakki.exe")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "jakki.exe")
else()
    set(CPACK_GENERATOR "AppImage")
    set(CPACK_PACKAGE_ICON "icon-256.png")
    set(CPACK_APPIMAGE_DESKTOP_FILE "jakki.desktop")
endif()

include(CPack)
