cmake_minimum_required(VERSION 3.16)
project(XtsyInjectorOnly LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
find_package(OpenGL REQUIRED)
find_program(XXD_EXECUTABLE xxd REQUIRED)
include(FetchContent)

set(INJECTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/injector")
set(PAYLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/payload")
set(PAYLOAD_CLIENT_DIR "${PAYLOAD_DIR}/client")

if(NOT EXISTS "${PAYLOAD_CLIENT_DIR}/dllmain.cpp")
    message(FATAL_ERROR "Missing payload client sources at payload/client")
endif()

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
    set(STB_IMAGE_HEADER "${THIRD_PARTY_DIR}/stb_image.h")
    set(STB_INCLUDE_DIR "${THIRD_PARTY_DIR}")
endif()

if(NOT EXISTS "${STB_IMAGE_HEADER}")
    message(FATAL_ERROR "stb_image.h not found at ${STB_IMAGE_HEADER}")
endif()

find_package(JNI QUIET)
if(NOT JNI_FOUND)
    if(DEFINED ENV{JAVA_HOME})
        set(JNI_INCLUDE_DIRS "$ENV{JAVA_HOME}/include" "$ENV{JAVA_HOME}/include/linux")
    else()
        message(FATAL_ERROR "JNI headers not found. Install JDK headers or set JAVA_HOME.")
    endif()
endif()

pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(EGL QUIET egl)
pkg_check_modules(LIBINPUT QUIET libinput)
pkg_check_modules(LIBUDEV QUIET libudev)

set(IMGUI_CORE_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
set(IMGUI_OPENGL3_SOURCE
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
set(IMGUI_OPENGL2_SOURCE
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)
set(IMGUI_SDL2_SOURCE
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
)

set(PAYLOAD_CLIENT_SO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/client.so")

add_library(client SHARED
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_OPENGL2_SOURCE}
    ${PAYLOAD_CLIENT_DIR}/dllmain.cpp
    ${PAYLOAD_CLIENT_DIR}/lion_native_bridge_stubs.cpp
    ${PAYLOAD_CLIENT_DIR}/gui/Menu.cpp
    ${PAYLOAD_CLIENT_DIR}/gui/Renderer.cpp
    ${PAYLOAD_CLIENT_DIR}/hooks/InputHook.cpp
)
target_include_directories(client PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${JNI_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
)
target_link_libraries(client PRIVATE
    dl
    pthread
    ${X11_LIBRARIES}
    OpenGL::GL
)
if(EGL_FOUND)
    target_link_libraries(client PRIVATE ${EGL_LIBRARIES})
    target_include_directories(client PRIVATE ${EGL_INCLUDE_DIRS})
    target_compile_definitions(client PRIVATE HAS_EGL=1)
endif()
if(LIBINPUT_FOUND AND LIBUDEV_FOUND)
    target_link_libraries(client PRIVATE ${LIBINPUT_LIBRARIES} ${LIBUDEV_LIBRARIES})
    target_include_directories(client PRIVATE ${LIBINPUT_INCLUDE_DIRS} ${LIBUDEV_INCLUDE_DIRS})
    target_compile_definitions(client PRIVATE HAS_LIBINPUT=1)
endif()
target_compile_options(client PRIVATE -fPIC -fvisibility=hidden -Wall -Wextra)
set_target_properties(client PROPERTIES
    PREFIX ""
    OUTPUT_NAME "client"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_custom_command(TARGET client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf"
            "$<TARGET_FILE_DIR:client>/Roboto-Medium.ttf"
)

set(EMBEDDED_CLIENT_SO_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_client_so.cpp")
add_custom_command(
    OUTPUT ${EMBEDDED_CLIENT_SO_CPP}
    COMMAND ${XXD_EXECUTABLE} -i -n g_embedded_client_so "${PAYLOAD_CLIENT_SO_OUTPUT}" "${EMBEDDED_CLIENT_SO_CPP}"
    DEPENDS client "${PAYLOAD_CLIENT_SO_OUTPUT}"
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
    ${IMGUI_OPENGL3_SOURCE}
    ${IMGUI_SDL2_SOURCE}
    ${INJECTOR_DIR}/main.cpp
    ${INJECTOR_DIR}/App.cpp
    ${INJECTOR_DIR}/ProcessScanner.cpp
    ${INJECTOR_DIR}/Injector.cpp
    ${INJECTOR_DIR}/Theme.cpp
    ${INJECTOR_DIR}/ImageLoader.cpp
)
add_dependencies(mc_injector client)

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
