cmake_minimum_required(VERSION 3.22)
project(monmix_unit_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Pure-logic unit tests. These build natively and run as ordinary exe's
# under ctest. No IDF, no LVGL, no SDL -- anything that wants those
# stays in the sim lane (tests/sim/).
#
# Each test_*.c is its own exe so a crash in one doesn't take down the
# whole suite. Add new tests by writing test_<name>.c and the
# add_unit_test below picks it up.

enable_testing()

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

function(add_unit_test name)
    add_executable(${name} ${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE
        ${MAIN_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    )
    if(MSVC)
        target_compile_options(${name} PRIVATE /wd4244 /wd4267 /wd4996)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_unit_test(test_db_taper)
add_unit_test(test_osc_expect ${MAIN_DIR}/osc_expect.c)
