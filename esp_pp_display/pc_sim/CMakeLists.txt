cmake_minimum_required(VERSION 3.22)
project(esp_pp_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(PP_ROOT     ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(MAIN_DIR    ${PP_ROOT}/main)
set(LVGL_DIR    ${PP_ROOT}/managed_components/lvgl__lvgl)

if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
    message(FATAL_ERROR
        "LVGL not found at ${LVGL_DIR}. Run an idf.py reconfigure on the "
        "esp_pp_display project first so managed_components/ is populated, "
        "then re-run cmake here.")
endif()

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

# cJSON for pp_client_real.c parsing. On the tablet this lands via the
# espressif/cjson registry component; on PC we vendor the upstream
# sources directly.
FetchContent_Declare(
    cjson
    URL      https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
    URL_HASH SHA256=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
)
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
    FetchContent_Populate(cjson)
endif()
set(CJSON_DIR ${cjson_SOURCE_DIR})

# LVGL static lib configured by our lv_conf.h (NOT IDF Kconfig).
file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
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

set(REUSED_MAIN_SRC
    ${MAIN_DIR}/app_ui.c
    ${MAIN_DIR}/app_pp_state.c
)

set(MOCK_SRC
    mocks/mock_app_pp_client.c
    mocks/mock_app_wifi.c
    mocks/mock_esp_lvgl_port.c
    mocks/mock_esp_system.c
    mocks/mock_esp_timer.c
    mocks/mock_freertos.c
)

set(SIM_SRC
    pp_client_real.c
    ${CJSON_DIR}/cJSON.c
)

add_executable(esp_pp_sim
    pc_main.c
    ${REUSED_MAIN_SRC}
    ${MOCK_SRC}
    ${SIM_SRC}
)

# Stubs first so #include "esp_log.h" etc. resolve to our shims.
target_include_directories(esp_pp_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MAIN_DIR}
    ${SDL2_INCLUDES}
    ${CJSON_DIR}
)

target_link_libraries(esp_pp_sim PRIVATE lvgl ${SDL2_LIBS})

if(WIN32)
    target_link_libraries(esp_pp_sim PRIVATE ws2_32)
endif()
if(NOT WIN32)
    target_link_libraries(esp_pp_sim PRIVATE m pthread)
endif()

if(MSVC)
    target_compile_options(esp_pp_sim PRIVATE
        /wd4244 /wd4267 /wd4996 /wd4018
        /Zi
        /FI${CMAKE_CURRENT_SOURCE_DIR}/sim_compat.h
    )
    target_link_options(esp_pp_sim PRIVATE /SUBSYSTEM:CONSOLE)
else()
    target_compile_options(esp_pp_sim PRIVATE
        -g -O0
        -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
        -Wno-missing-field-initializers
        -include ${CMAKE_CURRENT_SOURCE_DIR}/sim_compat.h
    )
endif()

if(WIN32)
    add_custom_command(TARGET esp_pp_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SDL2_RUNTIME_DLL} $<TARGET_FILE_DIR:esp_pp_sim>/SDL2.dll
    )
endif()
