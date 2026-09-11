cmake_minimum_required(VERSION 3.26.4 FATAL_ERROR)
project(project_irgpua LANGUAGES CXX CUDA)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RAFT_NVTX ON)
include(fetch_rapids.cmake)
include(rapids-cmake)
include(rapids-cpm)
include(rapids-cuda)
include(rapids-find)

rapids_cuda_init_architectures(Skels)

rapids_cpm_init()
include(cmake/thirdparty/get_cccl.cmake)
include(cmake/thirdparty/get_rmm.cmake)
include(cmake/thirdparty/get_raft.cmake)
include(cmake/thirdparty/get_nvtx3.cmake)

find_package(OpenMP REQUIRED)

# Set the CUDA standard
set(CMAKE_CUDA_STANDARD 20)
set(CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fconcepts")
#set(CUDA_ARCHITECTURES "75")
set(CMAKE_CUDA_ARCHITECTURES "native")

include_directories(src)

set(KERNEL_SRCS
    src/fix_cpu.cu
    src/gpu_part.cu
    src/gpu_part_indus.cu
)

#[[
# Executable 1: CPU implementation (fix_cpu)
add_executable(fix_cpu_exe
    src/main_fix_cpu.cu
    src/Utils.cu
    ${KERNEL_SRCS}
)
# Executable 2: GPU implementation ManoMano 1 (gpu_part)
add_executable(gpu_part_exe
    src/main_gpu_part.cu
    src/Utils.cu
    ${KERNEL_SRCS}
)
# Executable 3: GPU implementation Industrial (gpu_part_indus)
add_executable(gpu_part_indus_exe
    src/main_gpu_indus.cu
    src/Utils.cu
    ${KERNEL_SRCS}
)
set(EXECUTABLES
    fix_cpu_exe
    gpu_part_exe
    gpu_part_indus_exe
)
]]
foreach(TARGET ${EXECUTABLES})
    target_link_libraries(${TARGET} PUBLIC OpenMP::OpenMP_CXX rmm::rmm raft::raft nvtx3-cpp)
    target_compile_options(${TARGET} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=${OpenMP_CXX_FLAGS}>)
    target_link_options(${TARGET} PRIVATE $<$<BOOL:${CMAKE_CUDA_COMPILER_WORKS}>:-Xlinker,-linfo>)
    # Note: If your CUDA code in .cu files has -Xcompiler flags, you might need to adjust:
    # target_compile_options(${TARGET} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fPIC>)
endforeach()
