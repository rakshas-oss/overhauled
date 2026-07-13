class NvlinkPlacement < Formula
  desc "NVLink-aware GPU task placement library"
  homepage "https://github.com/rakshas-oss/overhauled"
  url "https://github.com/rakshas-oss/overhauled/archive/refs/tags/overhauled.tar.gz"
  version "1.0.0"
  sha256 "42470ef5e79d8dad6fa17b9803445976a5f83133559fd2726d2b372841e033c4"
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
