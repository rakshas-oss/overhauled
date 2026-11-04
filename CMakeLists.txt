# SPDX-License-Identifier: MIT
# Copyright (c) 2024 NVLink Placement Contributors

cmake_minimum_required(VERSION 3.18)
project(OverhauledTensorRuntime VERSION 1.1.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_CUDA "Enable CUDA acceleration when a CUDA compiler/toolkit is available" ON)
option(ENABLE_TENSORRT "Enable TensorRT runtime server" OFF)
option(BUILD_BROKER "Build broker interoperability service and server" ON)
option(BUILD_EXAMPLES "Build example binaries" ON)
option(ENABLE_INTEROP_ENCRYPTION "Enable interop encryption layer" OFF)
option(ENABLE_ETHOSU_DRM "Enable the mainline Linux Ethos-U DRM executor" OFF)

if(ENABLE_ETHOSU_DRM AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ENABLE_ETHOSU_DRM is supported only on Linux.")
endif()

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
add_library(Overhauled::nvlink_placement ALIAS nvlink_placement)
target_include_directories(nvlink_placement PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(nvlink_placement PUBLIC Threads::Threads)
if(HAS_CUDA)
    target_compile_definitions(nvlink_placement PUBLIC NVLINK_PLACEMENT_HAS_CUDA=1)
    target_link_libraries(nvlink_placement PUBLIC CUDA::cudart)
endif()

add_library(adi_server src/adi_server.cpp)
add_library(Overhauled::adi_server ALIAS adi_server)
target_include_directories(adi_server PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(adi_server PUBLIC nvlink_placement Threads::Threads)
if(HAS_CUBLAS)
    target_compile_definitions(adi_server PUBLIC NVLINK_ADI_HAS_CUBLAS=1)
    target_link_libraries(adi_server PUBLIC CUDA::cublas)
endif()

set(OVERHAULED_INSTALL_TARGETS nvlink_placement adi_server)
if(BUILD_BROKER)
    add_library(broker_service
        src/broker_protocol.cpp src/brk2_protocol.cpp src/broker_service.cpp
        src/broker_server.cpp src/geospatial_frame.cpp src/media_stream.cpp
        src/wasm_sandbox.cpp)
    add_library(Overhauled::broker_service ALIAS broker_service)
    target_include_directories(broker_service PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(broker_service PUBLIC adi_server nvlink_placement Threads::Threads)
    list(APPEND OVERHAULED_INSTALL_TARGETS broker_service)

    if(ENABLE_INTEROP_ENCRYPTION)
        add_library(interop_encryption src/interop_encryption.cpp)
        add_library(Overhauled::interop_encryption ALIAS interop_encryption)
        target_include_directories(interop_encryption PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_link_libraries(interop_encryption PUBLIC Threads::Threads)
        target_compile_definitions(interop_encryption PUBLIC OVERHAULED_INTEROP_ENCRYPTION=1)
        target_link_libraries(broker_service PUBLIC interop_encryption)
        list(APPEND OVERHAULED_INSTALL_TARGETS interop_encryption)
    endif()

    add_executable(broker_server src/broker_server_main.cpp)
    target_link_libraries(broker_server PRIVATE broker_service)
    install(TARGETS broker_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(ENABLE_ETHOSU_DRM)
        target_sources(broker_service PRIVATE src/ethosu_u85_drm_executor.cpp)
        target_compile_definitions(broker_server PRIVATE OVERHAULED_HAS_ETHOSU_DRM=1)
    endif()
endif()

if(HAS_TENSORRT)
    add_executable(gpu_runtime_server src/gpu_runtime_server.cpp)
    target_include_directories(gpu_runtime_server PRIVATE ${TENSORRT_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(gpu_runtime_server PRIVATE Threads::Threads ${NVINFER_LIB} ${NVONNXPARSER_LIB} CUDA::cuda_driver CUDA::cudart)
    install(TARGETS gpu_runtime_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
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
    foreach(test_name IN ITEMS brk2_protocol brk_conformance broker_protocol broker_service backend_dispatch broker_server_e2e broker_async_latency_benchmark geospatial_frame media_stream wasm_sandbox)
        add_executable(${test_name}_test tests/${test_name}_test.cpp)
        target_link_libraries(${test_name}_test PRIVATE broker_service Threads::Threads)
        add_test(NAME ${test_name}_test COMMAND ${test_name}_test)
    endforeach()
    target_compile_definitions(brk_conformance_test PRIVATE
        BRK_CONFORMANCE_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/protocol/brk-conformance-v1.corpus")
endif()

install(TARGETS ${OVERHAULED_INSTALL_TARGETS}
    EXPORT OverhauledTensorRuntimeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/overhauled/examples)
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/overhauled)

set(OVERHAULED_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/OverhauledTensorRuntime)
configure_package_config_file(
    cmake/OverhauledTensorRuntimeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/OverhauledTensorRuntimeConfig.cmake
    INSTALL_DESTINATION ${OVERHAULED_CMAKE_INSTALL_DIR})
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/OverhauledTensorRuntimeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT OverhauledTensorRuntimeTargets
    FILE OverhauledTensorRuntimeTargets.cmake
    NAMESPACE Overhauled::
    DESTINATION ${OVERHAULED_CMAKE_INSTALL_DIR})
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/OverhauledTensorRuntimeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/OverhauledTensorRuntimeConfigVersion.cmake
    DESTINATION ${OVERHAULED_CMAKE_INSTALL_DIR})

set(CPACK_PACKAGE_NAME "OverhauledTensorRuntime")
set(CPACK_PACKAGE_VENDOR "rakshas-oss")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "https://github.com/rakshas-oss/overhauled")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;ZIP")
if(UNIX AND NOT APPLE)
    list(APPEND CPACK_GENERATOR DEB RPM)
endif()
include(CPack)

message(STATUS "Build summary: HAS_CUDA=${HAS_CUDA}, HAS_CUBLAS=${HAS_CUBLAS}, HAS_TENSORRT=${HAS_TENSORRT}, BUILD_BROKER=${BUILD_BROKER}")
