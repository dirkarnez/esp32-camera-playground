# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)
add_compile_options(-fdiagnostics-color=always)

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project("esp32-camera-playground")
message(STATUS "Current IDF version is ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
