cmake_minimum_required(VERSION 3.16)
project(XtsyInjectorOnly LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
find_package(OpenGL REQUIRED)
find_program(XXD_EXECUTABLE xxd REQUIRED)
include(ExternalProject)
include(FetchContent)

set(INJECTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/injector")
option(XTSYINJECTOR_FETCH_THIRD_PARTY "Fetch ImGui and stb_image during configure" OFF)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(HAS_VENDORED_THIRD_PARTY OFF)
if(EXISTS "${THIRD_PARTY_DIR}/imgui/imgui.cpp" AND EXISTS "${THIRD_PARTY_DIR}/stb_image.h")
    set(HAS_VENDORED_THIRD_PARTY ON)
endif()

if(XTSYINJECTOR_FETCH_THIRD_PARTY OR NOT HAS_VENDORED_THIRD_PARTY)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/heads/docking.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")

    set(XTSYINJECTOR_DEPS_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps")
    file(MAKE_DIRECTORY "${XTSYINJECTOR_DEPS_DIR}")
    set(STB_IMAGE_HEADER "${XTSYINJECTOR_DEPS_DIR}/stb_image.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        "${STB_IMAGE_HEADER}"
        TLS_VERIFY ON
        STATUS STB_DOWNLOAD_STATUS
    )
    list(GET STB_DOWNLOAD_STATUS 0 STB_DOWNLOAD_CODE)
    if(NOT STB_DOWNLOAD_CODE EQUAL 0)
        list(GET STB_DOWNLOAD_STATUS 1 STB_DOWNLOAD_ERROR)
        message(FATAL_ERROR "Failed to download stb_image.h: ${STB_DOWNLOAD_ERROR}")
    endif()
    set(STB_INCLUDE_DIR "${XTSYINJECTOR_DEPS_DIR}")
else()
    set(IMGUI_DIR "${THIRD_PARTY_DIR}/imgui")
    set(STB_IMAGE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb_image.h")
    set(STB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()

if(NOT EXISTS "${STB_IMAGE_HEADER}")
    message(FATAL_ERROR "stb_image.h not found at ${STB_IMAGE_HEADER}")
endif()

set(IMGUI_CORE_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
set(IMGUI_OPENGL_SOURCE
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
set(IMGUI_SDL2_SOURCE
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
)

set(PAYLOAD_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PAYLOAD_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/_client_payload_build")
set(EMBED_CLIENT_SO_INPUT "${PAYLOAD_BUILD_DIR}/client.so")

set(PAYLOAD_CMAKE_ARGS "")
if(CMAKE_BUILD_TYPE)
    list(APPEND PAYLOAD_CMAKE_ARGS "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()

ExternalProject_Add(embedded_client_payload
    SOURCE_DIR "${PAYLOAD_SOURCE_DIR}"
    BINARY_DIR "${PAYLOAD_BUILD_DIR}"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -S <SOURCE_DIR> -B <BINARY_DIR> ${PAYLOAD_CMAKE_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target client
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${EMBED_CLIENT_SO_INPUT}"
    EXCLUDE_FROM_ALL TRUE
)

set(EMBEDDED_CLIENT_SO_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_client_so.cpp")
add_custom_command(
    OUTPUT ${EMBEDDED_CLIENT_SO_CPP}
    COMMAND ${XXD_EXECUTABLE} -i -n g_embedded_client_so "${EMBED_CLIENT_SO_INPUT}" "${EMBEDDED_CLIENT_SO_CPP}"
    DEPENDS embedded_client_payload "${EMBED_CLIENT_SO_INPUT}"
    VERBATIM
)

set(EMBED_LOGO_PNG_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png")
if(NOT EXISTS "${EMBED_LOGO_PNG_INPUT}")
    message(FATAL_ERROR "logo.png for embedding was not found at assets/logo.png")
endif()

set(EMBEDDED_LOGO_PNG_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_logo_png.cpp")
add_custom_command(
    OUTPUT ${EMBEDDED_LOGO_PNG_CPP}
    COMMAND ${XXD_EXECUTABLE} -i -n g_embedded_logo_png "${EMBED_LOGO_PNG_INPUT}" "${EMBEDDED_LOGO_PNG_CPP}"
    DEPENDS "${EMBED_LOGO_PNG_INPUT}"
    VERBATIM
)

add_executable(mc_injector
    ${EMBEDDED_CLIENT_SO_CPP}
    ${EMBEDDED_LOGO_PNG_CPP}
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_OPENGL_SOURCE}
    ${IMGUI_SDL2_SOURCE}
    ${INJECTOR_DIR}/main.cpp
    ${INJECTOR_DIR}/App.cpp
    ${INJECTOR_DIR}/ProcessScanner.cpp
    ${INJECTOR_DIR}/Injector.cpp
    ${INJECTOR_DIR}/Theme.cpp
    ${INJECTOR_DIR}/ImageLoader.cpp
)
add_dependencies(mc_injector embedded_client_payload)

target_include_directories(mc_injector PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${INJECTOR_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${STB_INCLUDE_DIR}
)

target_link_libraries(mc_injector PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
    dl
    cap
    pthread
)

target_compile_options(mc_injector PRIVATE -Wall -Wextra)

set_target_properties(mc_injector PROPERTIES
    OUTPUT_NAME "XtsyInjector"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build"
)
