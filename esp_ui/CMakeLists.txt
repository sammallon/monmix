# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_ui)

# Re-apply local hot-fixes to managed_components/ (gitignored, regenerated
# by `idf.py reconfigure`). See tools/apply_managed_patches.py for the
# rationale and the patches themselves.
execute_process(
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/tools/apply_managed_patches.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE _apply_patches_rc
)
if(NOT _apply_patches_rc EQUAL 0)
    message(FATAL_ERROR "apply_managed_patches.py failed (rc=${_apply_patches_rc})")
endif()
