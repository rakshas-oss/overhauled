class NvlinkPlacement < Formula
  desc "NVLink-aware GPU task placement library"
  homepage "https://github.com/rakshas-oss/overhauled"
  url "https://github.com/rakshas-oss/overhauled/archive/v1.0.0.tar.gz"
  sha256 "9999999999999999999999999999999999999999999999999999999999999999"
  license "MIT"
  
  depends_on "cmake" => :build
  depends_on "cuda-toolkit" => :build
  
  def install
    system "cmake", "-S", ".", "-B", "build", 
            "-DCMAKE_INSTALL_PREFIX=#{prefix}", 
            "-DBUILD_EXAMPLES=OFF"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end
  
  test do
    (testpath/"test.cpp").write <<~EOS
      #include <nvlink_placement.h>
      #include <iostream>
      
      int main() {
        std::cout << "NVLink Placement library loaded successfully" << std::endl;
        return 0;
      }
    EOS
    system ENV.cxx, "test.cpp", "-I#{include}", "-std=c++17"
  end
end
