class NvlinkPlacement < Formula
  desc "C++17 topology-aware placement library and broker runtime"
  homepage "https://github.com/rakshas-oss/overhauled"
  url "https://github.com/rakshas-oss/overhauled/archive/refs/tags/1.6.0.tar.gz"
  version "1.6.0"
  sha256 "399d29709086482db582de3ad4ecb1f1312925f415b238a036878e1f9863881d"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
           "-DBUILD_SHARED_LIBS=ON",
           "-DBUILD_BROKER=ON",
           "-DBUILD_EXAMPLES=OFF",
           "-DBUILD_TESTING=OFF",
           "-DENABLE_CUDA=OFF",
           "-DENABLE_TENSORRT=OFF",
           "-DENABLE_WASM_RUNTIME=OFF",
           "-DENABLE_WASM_HANDOFF=OFF"
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"CMakeLists.txt").write <<~CMAKE
      cmake_minimum_required(VERSION 3.18)
      project(OverhauledConsumer LANGUAGES CXX)
      find_package(OverhauledTensorRuntime CONFIG REQUIRED)
      add_executable(consumer consumer.cpp)
      target_link_libraries(consumer PRIVATE Overhauled::nvlink_placement)
    CMAKE
    (testpath/"consumer.cpp").write <<~CPP
      #include <nvlink_placement.h>
      int main() {
        auto topology = nvlink::GpuTopology::synthetic(2);
        nvlink::Placer placer(topology);
        return placer.assign_home(7) >= 0 ? 0 : 1;
      }
    CPP
    system "cmake", "-S", testpath, "-B", "build-consumer",
           "-DCMAKE_PREFIX_PATH=#{prefix}",
           "-DCMAKE_BUILD_TYPE=Release"
    system "cmake", "--build", "build-consumer"
    system "./build-consumer/consumer"
  end
end
