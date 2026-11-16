#include <bloom/update/refusal.hpp>

namespace bloom::update {

std::string_view refusalName(const UpdateRefusal refusal) noexcept {
    switch (refusal) {
    case UpdateRefusal::None:
        return "none";
    case UpdateRefusal::NotConfigured:
        return "not-configured";
    case UpdateRefusal::FeedTooLarge:
        return "feed-too-large";
    case UpdateRefusal::FeedMalformed:
        return "feed-malformed";
    case UpdateRefusal::FeedUnsigned:
        return "feed-unsigned";
    case UpdateRefusal::FeedUnknownKey:
        return "feed-unknown-key";
    case UpdateRefusal::FeedSignatureInvalid:
        return "feed-signature-invalid";
    case UpdateRefusal::FeedContentMalformed:
        return "feed-content-malformed";
    case UpdateRefusal::FeedChannelMismatch:
        return "feed-channel-mismatch";
    case UpdateRefusal::FeedAddressRefused:
        return "feed-address-refused";
    case UpdateRefusal::FileUnreadable:
        return "file-unreadable";
    case UpdateRefusal::FileSizeMismatch:
        return "file-size-mismatch";
    case UpdateRefusal::FileChecksumMismatch:
        return "file-checksum-mismatch";
    case UpdateRefusal::FileSignatureInvalid:
        return "file-signature-invalid";
    case UpdateRefusal::VerificationUnavailable:
        return "verification-unavailable";
    case UpdateRefusal::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace bloom::update
