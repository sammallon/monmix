cmake_minimum_required(VERSION 3.22)
project(monmix_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Tablet build (ESP-IDF) is unaffected — this CMakeLists.txt is invoked
# directly with native cmake from inside pc_sim/, never from esp_ui's
# top-level (which only includes project.cmake from $ENV{IDF_PATH}). And
# pc_sim/ isn't named "components" or registered in EXTRA_COMPONENT_DIRS,
# so IDF's component scanner never sees it.

set(MONMIX_ROOT     ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MAIN_DIR        ${MONMIX_ROOT}/main)
set(LVGL_DIR        ${MONMIX_ROOT}/managed_components/lvgl__lvgl)

if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
    message(FATAL_ERROR
        "LVGL not found at ${LVGL_DIR}. Run an idf.py reconfigure first so "
        "managed_components/ is populated, then re-run cmake here.")
endif()

# ──────────────────────────────────────────────────────────────────────
# SDL2 sourcing differs per platform:
#   Linux / WSL: prefer the system libsdl2-dev (apt install) via pkg-config.
#                Building SDL2 from source on a fresh WSL needs X11/wayland
#                dev headers anyway, so the apt path is strictly simpler.
#   Windows:    pull the prebuilt VC dev pack via FetchContent (unzipped
#                into the build dir; runtime DLL copied next to the exe).
# ──────────────────────────────────────────────────────────────────────
include(FetchContent)

if(WIN32)
    FetchContent_Declare(
        sdl2_devpack
        URL      https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-devel-2.30.10-VC.zip
        URL_HASH SHA256=a9adab9a570a72d2e4f1565dcf0275b7d9ef7b487e05ba0cf7a7a0f35c79c5e7
    )
    FetchContent_MakeAvailable(sdl2_devpack)

    # The 2.30.x VC dev pack extracts directly into the FetchContent
    # SOURCE_DIR (no nested SDL2-2.30.x subdir). Earlier versions did nest;
    # don't reintroduce that path component.
    set(SDL2_ROOT     ${sdl2_devpack_SOURCE_DIR})
    set(SDL2_INCLUDES ${SDL2_ROOT}/include)
    set(SDL2_LIBS     ${SDL2_ROOT}/lib/x64/SDL2.lib ${SDL2_ROOT}/lib/x64/SDL2main.lib)
    set(SDL2_RUNTIME_DLL ${SDL2_ROOT}/lib/x64/SDL2.dll)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(SDL2_INCLUDES ${SDL2_INCLUDE_DIRS})
    set(SDL2_LIBS PkgConfig::SDL2)
endif()

# ──────────────────────────────────────────────────────────────────────
# Mongoose — single-file C networking lib (WS client + HTTP client) used
# by ms_client_real.c when the user passes --ms-host. Vendored as source;
# we compile mongoose.c directly into the sim binary.
# ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    mongoose
    URL      https://github.com/cesanta/mongoose/archive/refs/tags/7.16.tar.gz
    URL_HASH SHA256=f2c42135f7bc34b3d10b6401e9326a20ba5dd42d4721b6a526826ba31c1679fd
)
FetchContent_MakeAvailable(mongoose)
set(MONGOOSE_DIR ${mongoose_SOURCE_DIR})

# ──────────────────────────────────────────────────────────────────────
# LVGL static library, configured from our pc_sim/lv_conf.h (and NOT
# from the ESP-IDF Kconfig generation that the tablet build uses).
# ──────────────────────────────────────────────────────────────────────
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
# Project sources reused from main/ — pure-C ones only. Anything that
# wants ESP-IDF gets a mock under mocks/.
# ──────────────────────────────────────────────────────────────────────
set(REUSED_MAIN_SRC
    ${MAIN_DIR}/app_ui.c
    ${MAIN_DIR}/app_state.c
    ${MAIN_DIR}/images/splash_logo.c
    ${MAIN_DIR}/fonts/font_monmix_level.c
)

set(MOCK_SRC
    mocks/mock_app_config.c
    mocks/mock_app_display.c
    mocks/mock_app_logd.c
    mocks/mock_app_ms_client.c
    mocks/mock_app_prefs.c
    mocks/mock_app_time.c
    mocks/mock_app_wifi.c
    mocks/mock_esp_lvgl_port.c
    mocks/mock_esp_netif_sntp.c
    mocks/mock_esp_system.c
    mocks/mock_esp_timer.c
    mocks/mock_freertos.c
)

set(SIM_SRC
    ms_client_real.c
    throttle.c
    ${MONGOOSE_DIR}/mongoose.c
)

add_executable(monmix_sim
    pc_main.c
    ${REUSED_MAIN_SRC}
    ${MOCK_SRC}
    ${SIM_SRC}
)

# Stubs come FIRST in the include path so #include "esp_log.h" etc resolve
# to our shims, not anything the system headers might also define.
target_include_directories(monmix_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MAIN_DIR}
    ${SDL2_INCLUDES}
    ${MONGOOSE_DIR}
)

# Mongoose configuration — keep TLS off (local MS is plain HTTP/WS), keep
# filesystem support off (we're a pure client), keep IPV6 off.
target_compile_definitions(monmix_sim PRIVATE
    MG_ENABLE_LINES=1
    MG_TLS=MG_TLS_NONE
    MG_ENABLE_PACKED_FS=0
    MG_ENABLE_FILE=0
    MG_ENABLE_IPV6=0
)

target_link_libraries(monmix_sim PRIVATE lvgl ${SDL2_LIBS})

if(WIN32)
    target_link_libraries(monmix_sim PRIVATE ws2_32)  # mongoose needs winsock
endif()

if(NOT WIN32)
    # libm for sqrtf etc; pthread because LVGL's misc helpers occasionally
    # touch __thread storage on glibc systems.
    target_link_libraries(monmix_sim PRIVATE m pthread)
endif()

if(MSVC)
    target_compile_options(monmix_sim PRIVATE
        /wd4244 /wd4267 /wd4996 /wd4018
        /Zi
        /FI${CMAKE_CURRENT_SOURCE_DIR}/sim_compat.h
    )
    target_link_options(monmix_sim PRIVATE /SUBSYSTEM:CONSOLE)
else()
    target_compile_options(monmix_sim PRIVATE
        -g -O0
        -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-missing-field-initializers
        -include ${CMAKE_CURRENT_SOURCE_DIR}/sim_compat.h
    )
endif()

if(WIN32)
    add_custom_command(TARGET monmix_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SDL2_RUNTIME_DLL} $<TARGET_FILE_DIR:monmix_sim>/SDL2.dll
    )
endif()
