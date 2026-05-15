cmake_minimum_required(VERSION 3.22)
project(stage_ui_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Invoked directly with native cmake from inside pc_sim/, never from a
# parent IDF build (there isn't one for stage_ui yet — that's the
# hardware round). pc_sim/ is not a "components" dir so ESP-IDF would
# never see it even if it existed.

set(STAGE_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MAIN_DIR    ${STAGE_ROOT}/main)

# ──────────────────────────────────────────────────────────────────────
# SDL2 — system pkg-config on Linux, prebuilt VC dev pack on Windows.
# ──────────────────────────────────────────────────────────────────────
include(FetchContent)

if(WIN32)
    FetchContent_Declare(
        sdl2_devpack
        URL      https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-devel-2.30.10-VC.zip
        URL_HASH SHA256=a9adab9a570a72d2e4f1565dcf0275b7d9ef7b487e05ba0cf7a7a0f35c79c5e7
    )
    FetchContent_MakeAvailable(sdl2_devpack)
    set(SDL2_ROOT        ${sdl2_devpack_SOURCE_DIR})
    set(SDL2_INCLUDES    ${SDL2_ROOT}/include)
    set(SDL2_LIBS        ${SDL2_ROOT}/lib/x64/SDL2.lib ${SDL2_ROOT}/lib/x64/SDL2main.lib)
    set(SDL2_RUNTIME_DLL ${SDL2_ROOT}/lib/x64/SDL2.dll)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(SDL2_INCLUDES ${SDL2_INCLUDE_DIRS})
    set(SDL2_LIBS     PkgConfig::SDL2)
endif()

# ──────────────────────────────────────────────────────────────────────
# LVGL — vendored via FetchContent so the sim doesn't depend on an IDF
# managed_components/ population. Pinned to a 9.4.x release that matches
# what esp_ui uses on hardware.
# ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.4.0
)
FetchContent_GetProperties(lvgl)
if(NOT lvgl_POPULATED)
    FetchContent_Populate(lvgl)
endif()
set(LVGL_DIR ${lvgl_SOURCE_DIR})

file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)

add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC
    ${LVGL_DIR}                  # for "lvgl.h"
    ${LVGL_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}  # for our lv_conf.h
    ${SDL2_INCLUDES}
)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE=1)
if(MSVC)
    target_compile_options(lvgl PRIVATE /wd4244 /wd4267 /wd4996 /wd4018)
else()
    target_compile_options(lvgl PRIVATE
        -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter
        -Wno-implicit-fallthrough -Wno-sign-compare
    )
endif()

# ──────────────────────────────────────────────────────────────────────
# Shared module sources (compile in the sim now; will compile on
# hardware in the IDF round).
# ──────────────────────────────────────────────────────────────────────
set(SHARED_MAIN_SRC
    ${MAIN_DIR}/app_pp_state.c
    ${MAIN_DIR}/app_pp_ui.c
    ${MAIN_DIR}/app_pp_ui_overlay.c
    ${MAIN_DIR}/app_pp_ui_settings.c
    ${MAIN_DIR}/app_prefs.c
    ${MAIN_DIR}/app_power.c
)

set(MOCK_SRC
    mocks/mock_app_pp_client.c
    mocks/mock_app_display.c
    mocks/mock_app_wifi.c
)

add_executable(stage_ui_sim
    pc_main.c
    ${SHARED_MAIN_SRC}
    ${MOCK_SRC}
)

target_include_directories(stage_ui_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs       # placeholder for hardware-round stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MAIN_DIR}
    ${SDL2_INCLUDES}
)

target_link_libraries(stage_ui_sim PRIVATE lvgl ${SDL2_LIBS})

if(WIN32)
    target_link_libraries(stage_ui_sim PRIVATE ws2_32)  # for socket fns in later rounds
endif()

if(NOT WIN32)
    target_link_libraries(stage_ui_sim PRIVATE m pthread)
endif()

if(MSVC)
    target_compile_options(stage_ui_sim PRIVATE
        /wd4244 /wd4267 /wd4996 /wd4018
        /Zi
        /FI${CMAKE_CURRENT_SOURCE_DIR}/sim_compat.h
    )
    target_link_options(stage_ui_sim PRIVATE /SUBSYSTEM:CONSOLE)
else()
    target_compile_options(stage_ui_sim PRIVATE
        -g -O0
        -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-missing-field-initializers
        -include ${CMAKE_CURRENT_SOURCE_DIR}/sim_compat.h
    )
endif()

if(WIN32)
    add_custom_command(TARGET stage_ui_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SDL2_RUNTIME_DLL} $<TARGET_FILE_DIR:stage_ui_sim>/SDL2.dll
    )
endif()
