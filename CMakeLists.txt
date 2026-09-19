cmake_minimum_required(VERSION 3.18)
project(OverhauledTensorRuntime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_CUDA "Enable CUDA acceleration when a CUDA compiler/toolkit is available" ON)
option(ENABLE_TENSORRT "Enable TensorRT runtime server" OFF)
option(BUILD_BROKER "Build broker interoperability service and server" ON)
option(BUILD_EXAMPLES "Build example binaries" ON)

include(CheckLanguage)
include(CTest)
find_package(Threads REQUIRED)

set(HAS_CUDA OFF)
set(HAS_CUBLAS OFF)
if(ENABLE_CUDA)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND)
            set(HAS_CUDA ON)
            if(TARGET CUDA::cublas)
                set(HAS_CUBLAS ON)
            endif()
            message(STATUS "CUDA enabled with toolkit ${CUDAToolkit_VERSION}")
        else()
            message(WARNING "CUDA compiler found but CUDAToolkit not found; building CPU-only.")
        endif()
    else()
        message(STATUS "CUDA compiler not found; building CPU-only.")
    endif()
else()
    message(STATUS "CUDA support disabled by ENABLE_CUDA=OFF.")
endif()

set(HAS_TENSORRT OFF)
if(ENABLE_TENSORRT)
    if(NOT HAS_CUDA)
        message(FATAL_ERROR "ENABLE_TENSORRT=ON requires CUDA support.")
    endif()

    set(TENSORRT_ROOT /usr/local/tensorrt CACHE PATH "Path to TensorRT root")
    find_path(TENSORRT_INCLUDE_DIR NvInfer.h
        HINTS ${TENSORRT_ROOT}/include /usr/include /usr/local/cuda/include)
    find_library(NVINFER_LIB nvinfer
        HINTS ${TENSORRT_ROOT}/lib /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)
    find_library(NVONNXPARSER_LIB nvonnxparser
        HINTS ${TENSORRT_ROOT}/lib /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)

    if(NOT TENSORRT_INCLUDE_DIR OR NOT NVINFER_LIB OR NOT NVONNXPARSER_LIB)
        message(FATAL_ERROR "TensorRT requested but not found. Set ENABLE_TENSORRT=OFF or provide TENSORRT_ROOT.")
    endif()

    set(HAS_TENSORRT ON)
    message(STATUS "TensorRT enabled from ${TENSORRT_ROOT}")
else()
    message(STATUS "TensorRT support disabled by default. Set ENABLE_TENSORRT=ON to build gpu_runtime_server.")
endif()

add_library(nvlink_placement src/nvlink_placement.cpp)
target_include_directories(nvlink_placement PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(nvlink_placement PUBLIC Threads::Threads)
if(HAS_CUDA)
    target_compile_definitions(nvlink_placement PUBLIC NVLINK_PLACEMENT_HAS_CUDA=1)
    target_link_libraries(nvlink_placement PUBLIC CUDA::cudart)
endif()

add_library(adi_server src/adi_server.cpp)
target_include_directories(adi_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(adi_server PUBLIC nvlink_placement Threads::Threads)
if(HAS_CUBLAS)
    target_compile_definitions(adi_server PUBLIC NVLINK_ADI_HAS_CUBLAS=1)
    target_link_libraries(adi_server PUBLIC CUDA::cublas)
endif()

if(BUILD_BROKER)
    add_library(broker_service
        src/broker_protocol.cpp
        src/broker_service.cpp
        src/broker_server.cpp)
    target_include_directories(broker_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(broker_service PUBLIC adi_server nvlink_placement Threads::Threads)

    add_executable(broker_server src/broker_server_main.cpp)
    target_link_libraries(broker_server PRIVATE broker_service)
endif()

if(HAS_TENSORRT)
    add_executable(gpu_runtime_server src/gpu_runtime_server.cpp)
    target_include_directories(gpu_runtime_server PRIVATE ${TENSORRT_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(gpu_runtime_server
        PRIVATE
        Threads::Threads
        ${NVINFER_LIB}
        ${NVONNXPARSER_LIB}
        CUDA::cuda_driver
        CUDA::cudart)
endif()

if(BUILD_EXAMPLES)
    add_executable(example_minimal examples/minimal.cpp)
    target_link_libraries(example_minimal PRIVATE nvlink_placement)

    add_executable(example_multi_client examples/multi_client.cpp)
    target_link_libraries(example_multi_client PRIVATE nvlink_placement)

    add_executable(example_adi_server examples/adi_server_example.cpp)
    target_link_libraries(example_adi_server PRIVATE adi_server)
endif()

if(BUILD_TESTING AND BUILD_BROKER)
    add_executable(broker_protocol_test tests/broker_protocol_test.cpp)
    target_link_libraries(broker_protocol_test PRIVATE broker_service)
    add_test(NAME broker_protocol_test COMMAND broker_protocol_test)

    add_executable(broker_service_test tests/broker_service_test.cpp)
    target_link_libraries(broker_service_test PRIVATE broker_service)
    add_test(NAME broker_service_test COMMAND broker_service_test)

    add_executable(broker_server_e2e_test tests/broker_server_e2e_test.cpp)
    target_link_libraries(broker_server_e2e_test PRIVATE broker_service Threads::Threads)
    add_test(NAME broker_server_e2e_test COMMAND broker_server_e2e_test)
endif()

install(TARGETS nvlink_placement adi_server DESTINATION lib)
if(BUILD_BROKER)
    install(TARGETS broker_service DESTINATION lib)
    install(TARGETS broker_server DESTINATION bin)
endif()
if(HAS_TENSORRT)
    install(TARGETS gpu_runtime_server DESTINATION bin)
endif()
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY examples/ DESTINATION examples)

message(STATUS "Build summary: HAS_CUDA=${HAS_CUDA}, HAS_CUBLAS=${HAS_CUBLAS}, HAS_TENSORRT=${HAS_TENSORRT}, BUILD_BROKER=${BUILD_BROKER}")
