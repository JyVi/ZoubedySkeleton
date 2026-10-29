cmake_minimum_required(VERSION 3.28)
project(skeletonize3d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
find_package(Threads REQUIRED)

option(BUILD_PYTHON_BINDINGS "Build Python bindings" ON)
option(BUILD_SKELETONIZE_APP "Build standalone executable" ON)

option(SKELETONIZE_NATIVE_OPTIMIZATION
       "Enable machine-specific CPU optimizations" ON)

option(SKELETONIZE_FAST_MATH
       "Enable aggressive floating-point optimizations" OFF)

function(skeletonize_apply_optimization target)
    target_compile_options(${target} PRIVATE
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-fexperimental-library>"
    )

    if(SKELETONIZE_NATIVE_OPTIMIZATION)
        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Release>:-march=native>"
        )
    endif()

    if(SKELETONIZE_FAST_MATH)
        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Release>:-ffast-math>"
        )
    endif()
endfunction()


FetchContent_Declare(
    bshoshany_thread_pool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG v5.1.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(bshoshany_thread_pool)

add_library(BS_thread_pool INTERFACE)

target_include_directories(BS_thread_pool SYSTEM INTERFACE
    ${bshoshany_thread_pool_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(skeletonize_core STATIC
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

target_compile_features(skeletonize_core PUBLIC cxx_std_20)

target_link_libraries(skeletonize_core PUBLIC
    #perfetto
    BS_thread_pool
)

skeletonize_apply_optimization(skeletonize_core)

# Propagate the experimental-library option to targets consuming the core.
target_link_options(skeletonize_core INTERFACE
    "$<$<CONFIG:Release>:-fexperimental-library>"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(skeletonize_core PRIVATE
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Debug>:-g3>"
        "$<$<CONFIG:Debug>:-Wall>"
        "$<$<CONFIG:Debug>:-Wextra>"
        "$<$<CONFIG:Debug>:-Werror>"
        "$<$<CONFIG:Debug>:-Wpedantic>"
    )
endif()

if(BUILD_PYTHON_BINDINGS)
    FetchContent_Declare(
          pybind11
          GIT_REPOSITORY https://github.com/pybind/pybind11.git
          GIT_TAG        v3.1.0
      )
    FetchContent_MakeAvailable(pybind11)

    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(_skeletonize_core MODULE NO_EXTRAS
        python/bindings.cc
    )

    target_link_libraries(_skeletonize_core PRIVATE
        skeletonize_core
    )

    skeletonize_apply_optimization(_skeletonize_core)

    install(TARGETS _skeletonize_core
        LIBRARY DESTINATION skeletonize3d
        RUNTIME DESTINATION skeletonize3d
    )
endif()

if(BUILD_SKELETONIZE_APP)
    add_executable(skeletonize_app src/main.cc)

    target_link_libraries(skeletonize_app PRIVATE
        skeletonize_core
    )

    skeletonize_apply_optimization(skeletonize_app)
endif()
