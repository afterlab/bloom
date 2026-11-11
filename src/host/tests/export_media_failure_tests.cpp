// #586: how a media export that published nothing is reported. A refused memory or processing
// admission (the provider's Busy, which is what a refused audio backing raises) is a resource
// refusal that names what was refused; only an output that outgrew its byte limit reads as a
// full destination.

#include <bloom/host/export_media_failure.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

} // namespace

int main() {
    using bloom::host::classifyExportMediaFailure;
    using bloom::host::ExportFailureCode;
    using bloom::host::exportFailureSummary;
    using bloom::host::exportMediaFailureReason;
    using bloom::media::provider::Error;

    const std::string_view diskFull = exportFailureSummary(ExportFailureCode::DestinationFull);
    const std::string audioRefusal = "Audio backing admission refused: not enough free memory or "
                                     "processing capacity for the decoded audio";

    const auto busy = classifyExportMediaFailure(Error::Busy);
    check(busy.code == ExportFailureCode::ResourceRefused && !busy.workerEnded,
          "a refused admission is a resource refusal");
    check(exportFailureSummary(busy.code) != diskFull,
          "a refused admission's headline is not the full-destination headline");
    const auto reason = exportMediaFailureReason(busy.code, audioRefusal, false);
    check(reason.find("Audio backing admission refused") != std::string::npos &&
              reason.find(diskFull) == std::string::npos &&
              reason.find("ran out of space") == std::string::npos && reason.back() == '.',
          "a refused audio backing reports that cause, never a full destination: " + reason);

    check(classifyExportMediaFailure(Error::Oversized).code == ExportFailureCode::DestinationFull,
          "an output past its byte limit is a full destination");
    check(classifyExportMediaFailure(Error::Timeout).code == ExportFailureCode::DeadlineExceeded,
          "a timeout is a deadline failure");
    check(classifyExportMediaFailure(Error::Io).code == ExportFailureCode::WriteFailed,
          "an I/O failure is a write failure");
    check(classifyExportMediaFailure(Error::Unavailable).code ==
              ExportFailureCode::EncoderUnavailable,
          "an unavailable encoder is named as unavailable");
    const auto crashed = classifyExportMediaFailure(Error::Crashed);
    check(crashed.code == ExportFailureCode::EncoderFailed && crashed.workerEnded,
          "a crashed worker is an encoder failure that says the worker ended");
    check(classifyExportMediaFailure(std::nullopt).code == ExportFailureCode::EncoderFailed &&
              classifyExportMediaFailure(Error::DigestMismatch).code ==
                  ExportFailureCode::EncoderFailed,
          "an unclassified failure stays an encoder failure");

    check(exportMediaFailureReason(ExportFailureCode::EncoderFailed, "technical detail", false) ==
              std::string(exportFailureSummary(ExportFailureCode::EncoderFailed)) +
                  " The details are in the log.",
          "a technical detail stays in the log");
    check(exportMediaFailureReason(ExportFailureCode::EncoderFailed, "A plain sentence.", true) ==
              "A plain sentence.",
          "a readable per-frame refusal is the reason as it stands");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
