#pragma once

#include <bloom/host/export_queue.hpp>
#include <bloom/media/provider/contract.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace bloom::host {

// How a media export that published nothing is reported to the artist. One mapping from the
// provider's reason to the job's failure code, so a memory or capacity refusal (Busy) can never
// read as a full destination (#586): only an output that outgrew its byte limit (Oversized) is
// `DestinationFull`.
struct ExportMediaFailureClass final {
    ExportFailureCode code = ExportFailureCode::EncoderFailed;
    // The media worker ended by itself (#583); the cause is plain and goes to the artist.
    bool workerEnded = false;
};

[[nodiscard]] constexpr ExportMediaFailureClass
classifyExportMediaFailure(const std::optional<media::provider::Error> reason) noexcept {
    using media::provider::Error;
    if (!reason)
        return {};
    switch (*reason) {
    case Error::Oversized:
        return {ExportFailureCode::DestinationFull, false};
    case Error::Timeout:
        return {ExportFailureCode::DeadlineExceeded, false};
    case Error::Busy:
        return {ExportFailureCode::ResourceRefused, false};
    case Error::Io:
        return {ExportFailureCode::WriteFailed, false};
    case Error::Unavailable:
        // An encoder or profile the worker cannot serve (#475) is a refusal with that name, not
        // an encoder that broke while running.
        return {ExportFailureCode::EncoderUnavailable, false};
    case Error::Crashed:
        return {ExportFailureCode::EncoderFailed, true};
    default:
        return {};
    }
}

// The reason sentence for an unclassified media failure. A per-frame refusal already carries a
// plain sentence (`readable`); a refused admission names what was refused, so its cause is shown
// with the headline; any other detail is technical and stays in the log.
[[nodiscard]] inline std::string exportMediaFailureReason(const ExportFailureCode code,
                                                          const std::string_view detail,
                                                          const bool readable) {
    if (readable)
        return std::string(detail);
    std::string reason(exportFailureSummary(code));
    if (code == ExportFailureCode::ResourceRefused && !detail.empty()) {
        reason.append(" ").append(detail);
        if (detail.back() != '.')
            reason.push_back('.');
        return reason;
    }
    return reason + " The details are in the log.";
}

} // namespace bloom::host
