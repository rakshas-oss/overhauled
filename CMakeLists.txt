cmake_minimum_required(VERSION 3.18)
project(OverhauledTensorRuntime LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

find_package(CUDA REQUIRED)

# TensorRT configuration
set(TENSORRT_ROOT /usr/local/tensorrt CACHE PATH "Path to TensorRT root")
find_path(TENSORRT_INCLUDE_DIR NvInfer.h HINTS ${TENSORRT_ROOT}/include /usr/include /usr/local/cuda/include)
find_library(NVINFER_LIB nvinfer HINTS ${TENSORRT_ROOT}/lib /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)
find_library(NVONNXPARSER_LIB nvonnxparser HINTS ${TENSORRT_ROOT}/lib /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)

if(NOT TENSORRT_INCLUDE_DIR OR NOT NVINFER_LIB OR NOT NVONNXPARSER_LIB)
    message(FATAL_ERROR "TensorRT not found. Please set TENSORRT_ROOT or install TensorRT.")
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CUDA_INCLUDE_DIRS}
    ${TENSORRT_INCLUDE_DIR}
)

# GPU Runtime Server
add_executable(gpu_runtime_server src/gpu_runtime_server.cpp)
target_link_libraries(gpu_runtime_server
    PRIVATE
    ${CUDA_LIBRARIES}
    ${NVINFER_LIB}
    ${NVONNXPARSER_LIB}
    cuda
    cudart
    pthread
)

# Installation targets
install(TARGETS gpu_runtime_server DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY examples/ DESTINATION examples)
