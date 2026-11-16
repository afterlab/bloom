#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace bloom::update {

enum class TransportStatus : std::uint8_t {
    Ok,
    Cancelled,
    // The server could not be reached: no network, no such host, a refused or lost connection.
    Offline,
    // The server answered with an error status.
    HttpError,
    // The answer was longer than the request allows; it was cut off.
    TooLarge,
    Failed,
};

// An inclusive range of bytes of the file at an address.
struct ByteRange final {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

struct TransportRequest final {
    std::string url;
    // The longest answer that is taken. A transfer is cut off as soon as it passes this.
    std::uint64_t maximumBytes = 0;
    // Only this part of the file, when the server gives parts (`partial` then says it did).
    std::optional<ByteRange> range;
};

struct TransportResult final {
    TransportStatus status = TransportStatus::Failed;
    int httpStatus = 0;
    // The answer is the requested range and not the whole file.
    bool partial = false;
    std::string error;
};

// Takes the body as it arrives. False stops the transfer.
using TransportSink = std::function<bool(std::span<const std::byte>)>;
using TransportProgress = std::function<void(std::uint64_t received, std::uint64_t total)>;

// How Bloom fetches the update feed and an update file: one plain GET of the address. An
// implementation sends nothing that says who asks: no query, no cookie, no identifier, and a
// fixed user agent. It blocks its caller, which is a background job and never the interface
// thread; `cancelled` is asked while it waits.
class UpdateTransport {
  public:
    UpdateTransport() = default;
    UpdateTransport(const UpdateTransport&) = delete;
    UpdateTransport& operator=(const UpdateTransport&) = delete;
    virtual ~UpdateTransport() = default;

    [[nodiscard]] virtual TransportResult get(const TransportRequest& request,
                                              const TransportSink& sink,
                                              const std::function<bool()>& cancelled,
                                              const TransportProgress& progress) = 0;
};

} // namespace bloom::update
