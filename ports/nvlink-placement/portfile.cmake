# SPDX-License-Identifier: MIT

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rakshas-oss/overhauled
    REF 1.6.0
    SHA512 bc809690c6cb092da7c84f53a06a40899a227b5ad1ffb08b6d4dac3965a6e12219509b05badd2e3cc44e3d006c41464a0e9044f49bcbc7dc33628dcd22437311
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_BROKER=ON
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TESTING=OFF
        -DENABLE_CUDA=OFF
        -DENABLE_NVLINK_HARDWARE_TESTS=OFF
        -DENABLE_TENSORRT=OFF
        -DENABLE_WASM_RUNTIME=OFF
        -DENABLE_WASM_HANDOFF=OFF
)

vcpkg_cmake_install()

# Relocate exported CMake package files to vcpkg's canonical share/<port>
# directory so find_package works for manifest and classic consumers.
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/OverhauledTensorRuntime)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)
vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/NOTICE"
)
