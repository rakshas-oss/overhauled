class NvlinkPlacement < Formula
  desc "NVLink-aware GPU task placement library"
  homepage "https://github.com/rakshas-oss/overhauled"
  # Immutable archive for the repository state published as version 1.0.0 metadata.
  url "https://github.com/rakshas-oss/overhauled/archive/c5178615521009d3f57eb7beeda80138d95687ee.tar.gz"
  version "1.0.0"
  sha256 "3e23ddf3b89915dbdf9a6605f72c4e1a1de3aa0370bf315cc0d6f11f755ab6bc"
  license "MIT"
  
  depends_on "cmake" => :build
  depends_on "cuda-toolkit"
  
  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args, "-DBUILD_EXAMPLES=OFF"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end
  
  test do
    (testpath/"CMakeLists.txt").write <<~EOS
      cmake_minimum_required(VERSION 3.18)
      project(nvlink-placement-test LANGUAGES CXX)

      find_package(nvlink_placement CONFIG REQUIRED)

      add_executable(test test.cpp)
      target_link_libraries(test PRIVATE nvlink_placement::nvlink_placement)
    EOS

    (testpath/"test.cpp").write <<~EOS
      #include <nvlink_placement.h>
      
      int main() {
        return 0;
      }
    EOS
    system "cmake", "-S", testpath, "-B", "build", "-DCMAKE_PREFIX_PATH=#{prefix}"
    system "cmake", "--build", "build"
  end
end
