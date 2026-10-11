# SPDX-License-Identifier: MIT
# Copyright (c) 2024 NVLink Placement Contributors

# CUDA/cuBLAS and TensorRT are optional, user-supplied NVIDIA system
# prerequisites. They are proprietary, governed by separate NVIDIA terms,
# and are not downloaded or redistributed by this port.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rakshas-oss/overhauled
    REF v1
    SHA512 7b27ddf09008568a5296444cf2438541c3d57046cc31584e383d19d6124888aec2658d0e0d85eec2cff2bca6248176a15138e183dfe6201225f37f16939bebb9
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

# Copy headers
file(INSTALL "${SOURCE_PATH}/include/" 
     DESTINATION "${CURRENT_PACKAGES_DIR}/include")

# Copy license
file(INSTALL "${SOURCE_PATH}/LICENSE" 
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/nvlink-placement" 
     RENAME copyright)
file(INSTALL "${SOURCE_PATH}/NOTICE"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/nvlink-placement")
