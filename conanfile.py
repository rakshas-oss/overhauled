# SPDX-License-Identifier: MIT
# Copyright (c) 2024 NVLink Placement Contributors

from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout
from conan.tools.files import copy


class NVLinkPlacementConan(ConanFile):
    name = "nvlink_placement"
    version = "1.1.1"
    package_type = "library"
    description = "NVLink-aware GPU task placement and broker runtime"
    author = "rakshas-oss"
    license = "MIT"
    url = "https://github.com/rakshas-oss/overhauled"
    homepage = "https://github.com/rakshas-oss/overhauled"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "cuda": [True, False],
        "broker": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "cuda": False,
        "broker": True,
    }

    generators = "CMakeDeps"
    exports_sources = "CMakeLists.txt", "cmake/*", "src/*", "include/*", "examples/*", "tests/*", "LICENSE", "NOTICE", "README.md"

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["ENABLE_CUDA"] = bool(self.options.cuda)
        tc.variables["ENABLE_TENSORRT"] = False
        tc.variables["BUILD_BROKER"] = bool(self.options.broker)
        tc.variables["BUILD_EXAMPLES"] = False
        tc.variables["BUILD_TESTING"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", self.source_folder, dst=self.package_folder, keep_path=False)
        copy(self, "NOTICE", self.source_folder, dst=self.package_folder, keep_path=False)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "OverhauledTensorRuntime")
        self.cpp_info.set_property("cmake_target_name", "Overhauled::nvlink_placement")
        self.cpp_info.libs = ["nvlink_placement", "adi_server"]
        if self.options.broker:
            self.cpp_info.libs.append("broker_service")
        if self.options.cuda:
            self.cpp_info.system_libs = ["cudart"]
