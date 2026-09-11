cmake_minimum_required(VERSION 3.14)
project(skeletonize3d LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(skeletonize_app src/main.cpp)
target_link_libraries(skeletonize_app)
