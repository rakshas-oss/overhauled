cmake_minimum_required(VERSION 3.18)
project(nvlink_placement VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CUDAToolkit QUIET)

add_library(nvlink_placement
    src/nvlink_placement.cpp
    src/adi_server.cpp
)

target_include_directories(nvlink_placement
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(CUDAToolkit_FOUND)
    target_link_libraries(nvlink_placement
        PUBLIC
            CUDA::cudart
    )
    target_compile_definitions(nvlink_placement PRIVATE NVLINK_PLACEMENT_HAS_CUDA=1)

    if(TARGET CUDA::cublas)
        target_link_libraries(nvlink_placement PUBLIC CUDA::cublas)
        target_compile_definitions(nvlink_placement PRIVATE NVLINK_ADI_HAS_CUBLAS=1)
    else()
        target_compile_definitions(nvlink_placement PRIVATE NVLINK_ADI_HAS_CUBLAS=0)
    endif()
else()
    message(STATUS "CUDA toolkit not found; building CPU-only fallbacks for GPU discovery and examples.")
    target_compile_definitions(nvlink_placement PRIVATE NVLINK_PLACEMENT_HAS_CUDA=0 NVLINK_ADI_HAS_CUBLAS=0)
endif()

target_compile_options(nvlink_placement
    PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Debug>:-g -Wall -Wextra>
)

option(BUILD_EXAMPLES "Build example programs" ON)

if(BUILD_EXAMPLES)
    add_executable(example_minimal
        examples/minimal.cpp
    )
    target_link_libraries(example_minimal PRIVATE nvlink_placement)

    add_executable(example_multi_client
        examples/multi_client.cpp
    )
    target_link_libraries(example_multi_client PRIVATE nvlink_placement)

    add_executable(example_adi_server
        examples/adi_server_example.cpp
    )
    target_link_libraries(example_adi_server PRIVATE nvlink_placement)
endif()

install(TARGETS nvlink_placement
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
