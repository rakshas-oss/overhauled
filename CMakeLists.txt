cmake_minimum_required(VERSION 3.18)
# The project uses only .cpp sources, so CUDA device-language compilation is not
# required here, but CUDA runtime and cuBLAS toolkit libraries are still linked.
project(nvlink_placement VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
add_library(nvlink_placement::nvlink_placement ALIAS nvlink_placement)

set_target_properties(nvlink_placement PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME nvlink_placement
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
    EXPORT nvlink_placementTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(nvlink_placement_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/nvlink_placement"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nvlink_placementConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nvlink_placementConfig.cmake"
    INSTALL_DESTINATION "${nvlink_placement_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/nvlink_placementConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT nvlink_placementTargets
    FILE nvlink_placementTargets.cmake
    NAMESPACE nvlink_placement::
    DESTINATION "${nvlink_placement_INSTALL_CMAKEDIR}"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/nvlink_placementConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/nvlink_placementConfigVersion.cmake"
    DESTINATION "${nvlink_placement_INSTALL_CMAKEDIR}"
)
