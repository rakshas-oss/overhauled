// SPDX-License-Identifier: MIT
// Mainline Linux DRM Ethos-U executor. UAPI layout follows
// include/uapi/drm/ethosu_accel.h from the Linux kernel.
#include "ethosu_u85_drm_executor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

namespace nvlink::broker {
namespace {

#if defined(__linux__)
constexpr unsigned kDrmCommandBase = 0x40;
constexpr unsigned kQuery = 0;
constexpr unsigned kBoCreate = 1;
constexpr unsigned kBoWait = 2;
constexpr unsigned kBoMmapOffset = 3;
constexpr unsigned kCmdstreamCreate = 4;
constexpr unsigned kSubmit = 5;
constexpr std::size_t kMaxRegions = 8;
constexpr std::uint32_t kMaxCommandBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxRegionBytes = 256U * 1024U * 1024U;
constexpr std::int64_t kJobTimeoutNs = 30LL * 1000LL * 1000LL * 1000LL;

struct DrmNpuInfo { std::uint32_t id, config, sram_size; };
struct DrmDevQuery { std::uint32_t type, size; std::uint64_t pointer; };
struct DrmBoCreate { std::uint64_t size; std::uint32_t flags, handle; };
struct DrmBoMmapOffset { std::uint32_t handle, pad; std::uint64_t offset; };
struct DrmBoWait { std::uint32_t handle, pad; std::int64_t timeout_ns; };
struct DrmCmdstreamCreate {
    std::uint32_t size, flags;
    std::uint64_t data;
    std::uint32_t handle, pad;
};
struct DrmJob {
    std::uint32_t cmd_bo, sram_size;
    std::uint32_t region_bo_handles[8];
};
struct DrmSubmit { std::uint64_t jobs; std::uint32_t job_count, pad; };

#define ETHOSU_IOCTL(id, type) _IOWR('d', kDrmCommandBase + (id), type)
constexpr unsigned long kIoctlQuery = ETHOSU_IOCTL(kQuery, DrmDevQuery);
constexpr unsigned long kIoctlBoCreate = ETHOSU_IOCTL(kBoCreate, DrmBoCreate);
constexpr unsigned long kIoctlBoWait = ETHOSU_IOCTL(kBoWait, DrmBoWait);
constexpr unsigned long kIoctlBoMmapOffset = ETHOSU_IOCTL(kBoMmapOffset, DrmBoMmapOffset);
constexpr unsigned long kIoctlCmdstreamCreate = ETHOSU_IOCTL(kCmdstreamCreate, DrmCmdstreamCreate);
constexpr unsigned long kIoctlSubmit = ETHOSU_IOCTL(kSubmit, DrmSubmit);
#undef ETHOSU_IOCTL

struct Mapping {
    void* address = MAP_FAILED;
    std::size_t size = 0;
    Mapping() = default;
    Mapping(int fd, std::uint32_t handle, std::size_t bytes) : size(bytes) {
        DrmBoMmapOffset req{handle, 0, 0};
        if (::ioctl(fd, kIoctlBoMmapOffset, &req) < 0)
            throw std::runtime_error("Ethos-U DRM BO_MMAP_OFFSET failed: " + std::string(std::strerror(errno)));
        address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         static_cast<off_t>(req.offset));
        if (address == MAP_FAILED)
            throw std::runtime_error("Ethos-U DRM mmap failed: " + std::string(std::strerror(errno)));
    }
    ~Mapping() { if (address != MAP_FAILED) ::munmap(address, size); }
    Mapping(Mapping&& other) noexcept : address(other.address), size(other.size) {
        other.address = MAP_FAILED; other.size = 0;
    }
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            if (address != MAP_FAILED) ::munmap(address, size);
            address = other.address; size = other.size;
            other.address = MAP_FAILED; other.size = 0;
        }
        return *this;
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
};

struct Bundle {
    std::uint32_t sram_size = 0;
    std::vector<std::uint8_t> command_stream;
    std::vector<std::vector<std::uint8_t>> regions;
    std::uint32_t input_region = 0, output_region = 0;
    std::uint32_t input_offset = 0, output_offset = 0;
    std::uint32_t input_elements = 0, output_elements = 0;
    float input_scale = 1.0F, output_scale = 1.0F;
    std::int32_t input_zero = 0, output_zero = 0;
};

std::uint32_t read_u32(std::istream& in) {
    std::array<unsigned char, 4> b{};
    if (!in.read(reinterpret_cast<char*>(b.data()), b.size()))
        throw std::runtime_error("truncated Ethos-U bundle header");
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
           (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
}
std::int32_t as_i32(std::uint32_t v) { std::int32_t out; std::memcpy(&out, &v, 4); return out; }
float as_f32(std::uint32_t v) { float out; std::memcpy(&out, &v, 4); return out; }

Bundle load_bundle(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open Ethos-U bundle: " + path);
    char magic[8]{};
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, "YU85BND1", 8) != 0)
        throw std::runtime_error("invalid Ethos-U bundle magic (expected YU85BND1)");
    const auto version = read_u32(in);
    if (version != 1) throw std::runtime_error("unsupported Ethos-U bundle version");
    Bundle b;
    b.sram_size = read_u32(in);
    const auto cmd_size = read_u32(in);
    const auto region_count = read_u32(in);
    b.input_region = read_u32(in); b.output_region = read_u32(in);
    b.input_offset = read_u32(in); b.output_offset = read_u32(in);
    b.input_elements = read_u32(in); b.output_elements = read_u32(in);
    b.input_scale = as_f32(read_u32(in)); b.input_zero = as_i32(read_u32(in));
    b.output_scale = as_f32(read_u32(in)); b.output_zero = as_i32(read_u32(in));
    if (cmd_size == 0 || cmd_size > kMaxCommandBytes || region_count == 0 ||
        region_count > kMaxRegions || b.sram_size == 0 ||
        b.input_region >= region_count || b.output_region >= region_count ||
        b.input_elements == 0 || b.output_elements == 0 ||
        !std::isfinite(b.input_scale) || b.input_scale <= 0 ||
        !std::isfinite(b.output_scale) || b.output_scale <= 0)
        throw std::runtime_error("invalid Ethos-U bundle dimensions or quantization");
    b.command_stream.resize(cmd_size);
    if (!in.read(reinterpret_cast<char*>(b.command_stream.data()), cmd_size))
        throw std::runtime_error("truncated Ethos-U command stream");
    b.regions.resize(region_count);
    for (auto& region : b.regions) {
        const auto size = read_u32(in);
        if (size == 0 || size > kMaxRegionBytes)
            throw std::runtime_error("invalid Ethos-U region size");
        region.resize(size);
        if (!in.read(reinterpret_cast<char*>(region.data()), size))
            throw std::runtime_error("truncated Ethos-U region data");
    }
    char extra;
    if (in.read(&extra, 1)) throw std::runtime_error("trailing bytes in Ethos-U bundle");
    const auto input_bytes = std::uint64_t(b.input_elements);
    const auto output_bytes = std::uint64_t(b.output_elements);
    if (std::uint64_t(b.input_offset) + input_bytes > b.regions[b.input_region].size() ||
        std::uint64_t(b.output_offset) + output_bytes > b.regions[b.output_region].size())
        throw std::runtime_error("Ethos-U tensor range exceeds its region");
    return b;
}

std::string find_device(const std::string& requested) {
    if (!requested.empty()) return requested;
    if (const char* env = std::getenv("ETHOSU_DRM_DEVICE")) {
        if (*env) return env;
    }
    for (int i = 128; i <= 255; ++i) {
        const std::string p = "/dev/dri/renderD" + std::to_string(i);
        if (::access(p.c_str(), R_OK | W_OK) == 0) return p;
    }
    return {};
}

int open_ethosu(const std::string& path) {
    if (path.empty()) return -1;
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    DrmNpuInfo info{};
    DrmDevQuery query{0, sizeof(info), reinterpret_cast<std::uint64_t>(&info)};
    if (::ioctl(fd, kIoctlQuery, &query) < 0) { ::close(fd); return -1; }
    return fd;
}
#endif // __linux__

} // namespace

struct EthosU85DrmExecutor::Impl {
    Bundle bundle;
    std::string path;
    mutable std::mutex mutex;
#if defined(__linux__)
    int fd = -1;
#endif
    Impl(const std::string& bundle_path, const std::string& device)
        : bundle(load_bundle(bundle_path)), path(find_device(device)) {
#if defined(__linux__)
        fd = open_ethosu(path);
#else
        (void)path;
#endif
    }
    ~Impl() {
#if defined(__linux__)
        if (fd >= 0) ::close(fd);
#endif
    }
};

EthosU85DrmExecutor::EthosU85DrmExecutor(const std::string& bundle_path,
                                         const std::string& device_path)
    : impl_(std::make_unique<Impl>(bundle_path, device_path)) {}
EthosU85DrmExecutor::~EthosU85DrmExecutor() = default;
const std::string& EthosU85DrmExecutor::device_path() const noexcept { return impl_->path; }

bool EthosU85DrmExecutor::available() const {
#if defined(__linux__)
    return impl_->fd >= 0;
#else
    return false;
#endif
}

std::vector<double> EthosU85DrmExecutor::run(const std::vector<double>& input) {
#if !defined(__linux__)
    (void)input;
    throw std::runtime_error("Ethos-U DRM execution is supported only on Linux");
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fd < 0) throw std::runtime_error("mainline Ethos-U DRM device is unavailable");
    const Bundle& b = impl_->bundle;
    if (input.size() != b.input_elements)
        throw std::runtime_error("input element count does not match Ethos-U bundle");

    std::vector<std::uint32_t> handles(b.regions.size(), 0);
    std::vector<Mapping> maps;
    maps.reserve(b.regions.size());
    for (std::size_t i = 0; i < b.regions.size(); ++i) {
        DrmBoCreate create{b.regions[i].size(), 0, 0};
        if (::ioctl(impl_->fd, kIoctlBoCreate, &create) < 0)
            throw std::runtime_error("Ethos-U BO_CREATE failed: " + std::string(std::strerror(errno)));
        handles[i] = create.handle;
        maps.emplace_back(impl_->fd, create.handle, b.regions[i].size());
        std::memcpy(maps.back().address, b.regions[i].data(), b.regions[i].size());
    }

    auto* input_data = static_cast<std::int8_t*>(maps[b.input_region].address) + b.input_offset;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (!std::isfinite(input[i])) throw std::runtime_error("Ethos-U input contains NaN or infinity");
        const double q = std::nearbyint(input[i] / b.input_scale) + b.input_zero;
        input_data[i] = static_cast<std::int8_t>(std::max(-128.0, std::min(127.0, q)));
    }

    DrmCmdstreamCreate cs{static_cast<std::uint32_t>(b.command_stream.size()), 0,
        reinterpret_cast<std::uint64_t>(b.command_stream.data()), 0, 0};
    if (::ioctl(impl_->fd, kIoctlCmdstreamCreate, &cs) < 0)
        throw std::runtime_error("Ethos-U CMDSTREAM_BO_CREATE failed: " + std::string(std::strerror(errno)));

    DrmJob job{};
    job.cmd_bo = cs.handle;
    job.sram_size = b.sram_size;
    for (std::size_t i = 0; i < handles.size(); ++i) job.region_bo_handles[i] = handles[i];
    DrmSubmit submit{reinterpret_cast<std::uint64_t>(&job), 1, 0};
    if (::ioctl(impl_->fd, kIoctlSubmit, &submit) < 0)
        throw std::runtime_error("Ethos-U SUBMIT failed: " + std::string(std::strerror(errno)));

    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        throw std::runtime_error("clock_gettime failed while waiting for Ethos-U job");
    const std::int64_t deadline = (std::int64_t(now.tv_sec) * 1000000000LL + now.tv_nsec) + kJobTimeoutNs;
    DrmBoWait wait{handles[b.output_region], 0, deadline};
    if (::ioctl(impl_->fd, kIoctlBoWait, &wait) < 0)
        throw std::runtime_error("Ethos-U BO_WAIT failed: " + std::string(std::strerror(errno)));

    const auto* output_data = static_cast<const std::int8_t*>(maps[b.output_region].address) + b.output_offset;
    std::vector<double> output(b.output_elements);
    for (std::size_t i = 0; i < output.size(); ++i)
        output[i] = (static_cast<int>(output_data[i]) - b.output_zero) * b.output_scale;
    return output;
#endif
}

} // namespace nvlink::broker
