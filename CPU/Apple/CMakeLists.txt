cmake_minimum_required(VERSION 3.14)
project(skeletonize3d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

add_library(skeletonize_core STATIC
    include/skeletonize/pipeline.hpp
    include/skeletonize/pipeline.cc
    include/skeletonize/lut.hpp
)

target_include_directories(skeletonize_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(skeletonize_core PRIVATE
    # Debug: No optimization, full debug symbols, enable all warnings
    $<$<CONFIG:Debug>:-O0 -g3 -Wall -Wextra -Wpedantic>

    # Release: Maximum optimization, native CPU instructions, disable asserts
    $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
)

add_executable(skeletonize_app src/main.cc)
target_link_libraries(skeletonize_app PRIVATE skeletonize_core)
