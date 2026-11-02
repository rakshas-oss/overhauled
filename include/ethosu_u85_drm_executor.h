// SPDX-License-Identifier: MIT
#pragma once

#include "backend_dispatch.h"

#include <memory>
#include <string>

namespace nvlink::broker {

// Executes Vela-compiled Ethos-U command streams through the mainline Linux
// DRM Ethos-U UAPI. The model bundle contract is documented in
// docs/ETHOSU_U85_BUNDLE.md.
class EthosU85DrmExecutor final : public NpuExecutor {
public:
    explicit EthosU85DrmExecutor(const std::string& bundle_path,
                                 const std::string& device_path = {});
    ~EthosU85DrmExecutor() override;

    EthosU85DrmExecutor(const EthosU85DrmExecutor&) = delete;
    EthosU85DrmExecutor& operator=(const EthosU85DrmExecutor&) = delete;

    bool available() const override;
    std::vector<double> run(const std::vector<double>& input) override;
    const std::string& device_path() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nvlink::broker
