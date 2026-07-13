vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rakshas-oss/overhauled
    REF v1.0.0
    SHA512 0
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
