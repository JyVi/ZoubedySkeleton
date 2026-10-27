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
find_package(Threads)

FetchContent_Declare(perfetto_sdk URL
                     https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip)

FetchContent_MakeAvailable(perfetto_sdk)

add_library(perfetto STATIC
    ${perfetto_sdk_SOURCE_DIR}/perfetto.cc
)

target_include_directories(perfetto PUBLIC
    ${perfetto_sdk_SOURCE_DIR}
)

target_compile_features(perfetto PUBLIC cxx_std_17)
target_link_libraries(perfetto PUBLIC Threads::Threads)

FetchContent_Declare(
    bshoshany_thread_pool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG v5.1.0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(bshoshany_thread_pool)
add_library(BS_thread_pool INTERFACE)
target_include_directories(BS_thread_pool INTERFACE
                           ${bshoshany_thread_pool_SOURCE_DIR}/include)

add_library(skeletonize_core STATIC
    include/skeletonize/perfetto_tracing.hpp
    include/skeletonize/perfetto_tracing.cc
    include/skeletonize/pipeline.hpp
    include/skeletonize/pipeline.cc
    include/skeletonize/lut.hpp
    include/skeletonize/helpers.cc
    include/skeletonize/helpers.hpp
    include/skeletonize/helpers_bit.cc
    include/skeletonize/helpers_bit.hpp
    include/skeletonize/pipeline_multi_thread.cc
    include/skeletonize/pipeline_multi_thread.hpp
    include/skeletonize/pipeline_single_thread.cc
    include/skeletonize/pipeline_single_thread.hpp
    include/skeletonize/pipeline_single_thread_bit.cc
    include/skeletonize/pipeline_single_thread_bit.hpp
    include/skeletonize/pipeline_multi_thread_bit.cc
    include/skeletonize/pipeline_multi_thread_bit.hpp
)

target_include_directories(skeletonize_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(skeletonize_core PRIVATE
                       # Debug: No optimization, full debug symbols,
                       # enable all warnings
                       $<$<CONFIG:Debug>:-O0 -g3 -Wall -Wextra -Werror
                       -Wpedantic>
                       # Release: Maximum optimization, native CPU instructions,
                       # disable asserts
                       $<$<CONFIG:Release>:-O3 -march=native -ffast-math
                       -DNDEBUG -fexperimental-library> #-fsanitize=thread>
                       $<$<CONFIG:RelWithDebInfo>:-O0 -g -fno-inline
                       -fno-omit-frame-pointer -DNDEBUG>)

#target_link_options(skeletonize_core INTERFACE
#    $<$<CONFIG:Release>:-fsanitize=thread>
#)
target_link_options(skeletonize_core INTERFACE
    $<$<CONFIG:Release>:-fexperimental-library>
)

target_link_libraries(skeletonize_core PUBLIC
  perfetto
)
target_link_libraries(skeletonize_core PUBLIC BS_thread_pool)

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

# This executable is the one used for debugging outside of python
# add_executable(test_skeleton tests/Cpp/main.cpp)
# target_link_libraries(test_skeleton PRIVATE skeletonize_core)
