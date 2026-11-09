#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bloom::runtime::fonts {

enum class FontHttpStatus : std::uint8_t {
    Ok,
    // No network, a name that does not resolve, or a refused connection.
    Offline,
    // The server answered with an error status (httpStatus).
    HttpError,
    Cancelled,
    TooLarge,
    Failed,
};

struct FontHttpResult final {
    FontHttpStatus status = FontHttpStatus::Failed;
    int httpStatus = 0;
    std::vector<std::uint8_t> body;
    std::string error;
};

// The one network seam of the font service. It performs a blocking GET on the calling thread (the
// font worker, never the UI thread) and must poll `cancelled` while it waits and call `progress`
// with the bytes received and the expected total (0 when unknown). The desktop application supplies
// the implementation over the platform's secure transport; tests supply a fake.
class FontTransport {
  public:
    virtual ~FontTransport() = default;
    [[nodiscard]] virtual FontHttpResult
    get(const std::string& url, std::uint64_t maximumBytes, const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t received, std::uint64_t total)>& progress) = 0;
};

} // namespace bloom::runtime::fonts
