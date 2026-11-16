#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bloom::update {

// Why an update, or the list that offers it, was not accepted. Every refusal is final for that
// attempt: nothing refused is installed, shown as available or retried under weaker rules.
enum class UpdateRefusal : std::uint8_t {
    None,
    NotConfigured,       // this build carries no update address or no public key
    FeedTooLarge,        // the list is larger than a list can be
    FeedMalformed,       // not the signed envelope the list is published in
    FeedUnsigned,        // the envelope carries no signature
    FeedUnknownKey,      // no signature is by a key this build carries
    FeedSignatureInvalid, // a signature by a carried key does not match the content
    FeedContentMalformed, // signed, but not a list this version understands
    FeedChannelMismatch, // signed, for another channel than this build's
    FeedAddressRefused,  // signed, but names an address that is not HTTPS
    FileUnreadable,      // the downloaded file cannot be read
    FileSizeMismatch,    // the file is not the size the list gives
    FileChecksumMismatch, // the file's SHA-256 is not the one the list gives
    FileSignatureInvalid, // the list's signature for the file does not match it
    VerificationUnavailable, // this copy cannot check signatures (a file of its package is missing)
    Cancelled,
};

[[nodiscard]] std::string_view refusalName(UpdateRefusal refusal) noexcept;

// A refusal with the sentence that explains it in a diagnostic.
struct UpdateDiagnostic final {
    UpdateRefusal refusal = UpdateRefusal::None;
    std::string detail;

    [[nodiscard]] bool accepted() const noexcept { return refusal == UpdateRefusal::None; }
};

} // namespace bloom::update
