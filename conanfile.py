from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout
from conan.tools.files import copy


class NVLinkPlacementConan(ConanFile):
    name = "nvlink_placement"
    version = "0.3.0"
    description = "NVLink-aware GPU task placement library"
    author = "rakshas-oss"
    license = "MIT"
    url = "https://github.com/rakshas-oss/overhauled"
    homepage = "https://github.com/rakshas-oss/overhauled"
    
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }
    
    generators = "CMakeDeps"
    exports_sources = "CMakeLists.txt", "src/*", "include/*", "LICENSE", "README.md"
    
    requires = ()
    
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
        tc.generate()
    
    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
    
    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", self.source_folder, 
             dst=self.package_folder, keep_path=False)
    
    def package_info(self):
        self.cpp_info.libs = ["nvlink_placement"]
        self.cpp_info.includedirs = ["include"]
        # CUDA dependency
        self.cpp_info.system_libs = ["cudart"]
    
    def set_version(self):
        self.version = "0.3.0"
