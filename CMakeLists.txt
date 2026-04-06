cmake_minimum_required(VERSION 3.16)

project(middleware LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_subdirectory(src/dcms)
add_subdirectory(src/hal)
add_subdirectory(src/dlog)

option(MIDDLEWARE_BUILD_TESTS "Build middleware unit tests" ON)
if(MIDDLEWARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(MIDDLEWARE_BUILD_EXAMPLES "Build example / practice programs" ON)
if(MIDDLEWARE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
