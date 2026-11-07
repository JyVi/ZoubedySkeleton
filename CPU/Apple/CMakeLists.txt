
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

if(APPLE)

    set(SKELETONIZE_PLATFORM "APPLE")

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "Apple configuration requires Clang/AppleClang.")
    endif()

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    set(SKELETONIZE_PLATFORM "LINUX")

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "Linux configuration requires GCC or Clang.")
    endif()

else()

    message(FATAL_ERROR
        "Unsupported platform: ${CMAKE_SYSTEM_NAME}")

endif()

message(STATUS "Skeletonize platform: ${SKELETONIZE_PLATFORM}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compiler version: ${CMAKE_CXX_COMPILER_VERSION}")

option(BUILD_PYTHON_BINDINGS
       "Build Python bindings" ON)

option(BUILD_SKELETONIZE_APP
       "Build standalone executable" ON)

option(SKELETONIZE_NATIVE_OPTIMIZATION
       "Enable machine-specific CPU optimizations" ON)

option(SKELETONIZE_FAST_MATH
       "Enable aggressive floating-point optimizations" OFF)

option(SKELETONIZE_USE_SYSTEM_TBB
       "Prefer a system-installed oneTBB on Linux" ON)

function(skeletonize_apply_optimization target)

    target_compile_options(${target} PRIVATE
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-g>"
    )
    #target_compile_options(
    #    ${target} PRIVATE -ftime-report
    #)

    if(SKELETONIZE_PLATFORM STREQUAL "APPLE")

        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Release>:-fexperimental-library>"
        )

    endif()

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

find_path(BS_THREAD_POOL_INCLUDE_DIR
    NAMES BS_thread_pool.hpp
)

if(BS_THREAD_POOL_INCLUDE_DIR)

    message(STATUS
        "Found BS::thread_pool: ${BS_THREAD_POOL_INCLUDE_DIR}")

else()

    message(STATUS
        "BS::thread_pool not found. Fetching v5.1.0.")

    FetchContent_Declare(
        bshoshany_thread_pool
        GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
        GIT_TAG v5.1.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
        SYSTEM
    )

    FetchContent_MakeAvailable(bshoshany_thread_pool)

    set(BS_THREAD_POOL_INCLUDE_DIR
        "${bshoshany_thread_pool_SOURCE_DIR}/include"
    )

endif()

add_library(BS_thread_pool INTERFACE)

target_include_directories(BS_thread_pool SYSTEM INTERFACE
    "${BS_THREAD_POOL_INCLUDE_DIR}"
)

if(SKELETONIZE_PLATFORM STREQUAL "LINUX")
    if(SKELETONIZE_USE_SYSTEM_TBB)
        find_package(TBB CONFIG QUIET)
    endif()

    if(NOT TARGET TBB::tbb)
        include(FetchContent)

        set(TBB_TEST OFF CACHE BOOL "" FORCE)
        set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
        set(TBBMALLOC_PROXY_BUILD OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            oneTBB
            GIT_REPOSITORY https://github.com/oneapi-src/oneTBB.git
            GIT_TAG v2023.1.0
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )

        FetchContent_MakeAvailable(oneTBB)
    endif()
endif()

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
    BS_thread_pool
    Threads::Threads
)

if(SKELETONIZE_PLATFORM STREQUAL "LINUX")

    target_link_libraries(skeletonize_core PUBLIC
        TBB::tbb
    )
    install(TARGETS TBB::tbb
        LIBRARY DESTINATION skeletonize3d
    )
    set_target_properties(_skeletonize_core PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH TRUE
        )

endif()

skeletonize_apply_optimization(skeletonize_core)

if(SKELETONIZE_PLATFORM STREQUAL "APPLE")

    target_link_options(skeletonize_core INTERFACE
        "$<$<CONFIG:Release>:-fexperimental-library>"
    )

endif()

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

    find_package(Python COMPONENTS
        Interpreter
        Development.Module
        REQUIRED
    )

    set(PYBIND11_FINDPYTHON ON)

    find_package(pybind11 3.1 CONFIG QUIET)

    if(NOT pybind11_FOUND)

        message(STATUS "pybind11 not found. Fetching v3.1.0.")

        FetchContent_Declare(
            pybind11
            GIT_REPOSITORY https://github.com/pybind/pybind11.git
            GIT_TAG v3.1.0
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )

        FetchContent_MakeAvailable(pybind11)

    else()

        message(STATUS
            "Found pybind11: ${pybind11_VERSION}")

    endif()

    pybind11_add_module(_skeletonize_core
        python/bindings.cc
    )

    target_link_libraries(_skeletonize_core PRIVATE
        skeletonize_core
    )

    #skeletonize_apply_optimization(skeletonize_core)

    install(TARGETS _skeletonize_core
        LIBRARY DESTINATION skeletonize3d
        RUNTIME DESTINATION skeletonize3d
    )

endif()

if(BUILD_SKELETONIZE_APP)

    add_executable(skeletonize_app
        tests/Cpp/main.cpp
    )

    target_link_libraries(skeletonize_app PRIVATE
        skeletonize_core
    )

    skeletonize_apply_optimization(skeletonize_app)

endif()
