cmake_minimum_required(VERSION 3.18)
project(nvlink_placement VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find CUDA toolkit libraries used by the C++ sources
find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)

# Main Library
add_library(nvlink_placement
    src/nvlink_placement.cpp
    src/adi_server.cpp
)

target_include_directories(nvlink_placement
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nvlink_placement
    PUBLIC
        CUDA::cudart
        CUDA::cublas
        Threads::Threads
)

target_compile_options(nvlink_placement
    PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Debug>:-g -Wall -Wextra>
)

# Examples
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

# Installation
install(TARGETS nvlink_placement
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
