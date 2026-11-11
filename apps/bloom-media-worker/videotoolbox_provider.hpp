#pragma once

#include <bloom/media/provider/protocol.hpp>

#include <memory>
#include <string>

namespace bloom::media::videotoolbox {

// Encode and mux through AVAssetWriter and VideoToolbox. Reading is videotoolbox_reader.hpp.
class Encoder final {
  public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    [[nodiscard]] provider::Payload call(provider::MessageKind, const provider::Payload&);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] bool hardwareEncodeAvailable();

} // namespace bloom::media::videotoolbox
