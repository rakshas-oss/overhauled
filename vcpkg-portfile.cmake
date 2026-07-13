vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO rakshas-oss/overhauled
    REF overhauled
    SHA512 93a7a05add7bca64ddad8f85860f72e789b7a62acfd7a767e0c6b1b279363f28a68648aa9f052c49a0599b04a6e01cb16c76964cad7b76174995323fe3c85781
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
