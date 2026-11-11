#pragma once

// The helper's side of the config request group (ocio_config_protocol.hpp): the only place a
// project-relative archive, an external archive or an external loose config is parsed, and where
// its processors are built and applied. lut_worker.cpp owns the process, the handshake and the
// platform's confinement, and hands every config command to one ConfigWorker.

#include "lut_worker_protocol.hpp"

#include <cstdint>
#include <memory>

namespace bloom::color::detail {

class ConfigWorker final {
  public:
    ConfigWorker();
    ~ConfigWorker();
    ConfigWorker(const ConfigWorker&) = delete;
    ConfigWorker& operator=(const ConfigWorker&) = delete;

    // Serves one config command. `confine` is the platform's confinement, entered before the
    // first untrusted byte is read. A result other than None is a protocol-level failure after
    // which the helper exits; a request the config cannot satisfy is answered in its reply
    // record instead. `payload` is the reply's byte count.
    [[nodiscard]] LutError handle(const LutPacket& message, int descriptorSocket, bool (*confine)(),
                                  std::uint64_t& payload);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::color::detail
