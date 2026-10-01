cmake_minimum_required(VERSION 3.19)
project(skeletonize3d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

include(FetchContent)

FetchContent_Declare(
    perfetto_sdk
    URL https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip
)

FetchContent_MakeAvailable(perfetto_sdk)

add_library(perfetto STATIC
    ${perfetto_sdk_SOURCE_DIR}/perfetto.cc
)

target_include_directories(perfetto PUBLIC
    ${perfetto_sdk_SOURCE_DIR}
)

target_compile_features(perfetto PUBLIC cxx_std_17)

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
    $<$<CONFIG:Debug>:-O0 -g3 -Wall -Wextra -Werror -Wpedantic>

    # Release: Maximum optimization, native CPU instructions, disable asserts
    $<$<CONFIG:Release>:-O3 -march=native -ffast-math -DNDEBUG>

    $<$<CONFIG:RelWithDebInfo>:-O0 -g -fno-inline -fno-omit-frame-pointer -DNDEBUG>

)

target_link_libraries(skeletonize_core PRIVATE
  perfetto
)

option(BUILD_PYTHON_BINDINGS "Build Python bindings" ON)
if(BUILD_PYTHON_BINDINGS)
  # Fetch pybind11 automatically
  include(FetchContent)
  FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v3.1.0
    )
  FetchContent_MakeAvailable(pybind11)

  pybind11_add_module(_skeletonize_core python/bindings.cc)
  target_link_libraries(_skeletonize_core PRIVATE skeletonize_core)

  install(TARGETS _skeletonize_core LIBRARY DESTINATION
            ${CMAKE_CURRENT_SOURCE_DIR}/python/skeletonize3d)
endif()

add_executable(skeletonize_app src/main.cc)
target_link_libraries(skeletonize_app PRIVATE skeletonize_core)
