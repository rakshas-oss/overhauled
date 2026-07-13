vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rakshas-oss/overhauled
    REF c5178615521009d3f57eb7beeda80138d95687ee
    SHA512 171dd70dbaaa23708601f01461e2bc6ee105cbd9bd04a032508d511589f9a97dc0120ee1f6204dc938ffc3d2cfecab795e53d4b28423a2c4d4ddbb4d6bcea8f2
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME nvlink_placement CONFIG_PATH lib/cmake/nvlink_placement)

vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# Copy license
file(INSTALL "${SOURCE_PATH}/LICENSE" 
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/nvlink-placement" 
     RENAME copyright)
