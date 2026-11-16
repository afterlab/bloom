#include <bloom/update/check.hpp>

#include <fstream>
#include <vector>

namespace bloom::update {

CheckResult checkForUpdate(const UpdateConfiguration& configuration, UpdateTransport& transport,
                           const ReleaseVersion& current, const InstallKind install,
                           const std::string_view platformKey,
                           const std::function<bool()>& cancelled) {
    CheckResult result;
    if (!configuration.configured()) {
        result.diagnostic = {UpdateRefusal::NotConfigured,
                             "this build carries no update address or key"};
        return result;
    }
    std::vector<std::byte> body;
    TransportRequest request;
    request.url = configuration.feedUrl;
    request.maximumBytes = kMaximumFeedBytes;
    result.transport = transport.get(
        request,
        [&body](const std::span<const std::byte> bytes) {
            body.insert(body.end(), bytes.begin(), bytes.end());
            return body.size() <= kMaximumFeedBytes;
        },
        cancelled, {});
    if (result.transport.status == TransportStatus::Cancelled) {
        result.status = CheckStatus::Cancelled;
        return result;
    }
    if (result.transport.status == TransportStatus::TooLarge) {
        result.status = CheckStatus::Refused;
        result.diagnostic = {UpdateRefusal::FeedTooLarge, "the feed is larger than a feed can be"};
        return result;
    }
    if (result.transport.status != TransportStatus::Ok) {
        result.status = CheckStatus::Unreachable;
        return result;
    }
    auto read = readSignedFeed(
        body, {configuration.keys, current.channel(), configuration.testBuild});
    if (!read.feed.has_value()) {
        result.status = CheckStatus::Refused;
        result.diagnostic = std::move(read.diagnostic);
        return result;
    }
    result.status = CheckStatus::Decided;
    result.decision = decideUpdate(*read.feed, current, install, platformKey);
    result.feed = std::move(read.feed);
    return result;
}

DownloadResult downloadUpdateFile(UpdateTransport& transport, const FeedFile& entry,
                                  const std::span<const Ed25519PublicKey> trustedKeys,
                                  const std::filesystem::path& destination,
                                  const std::function<bool()>& cancelled,
                                  const TransportProgress& progress) {
    DownloadResult result;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    auto partial = destination;
    partial += ".partial";
    const auto discard = [&partial] {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
    };
    std::filesystem::remove(destination, error);
    UpdateFileVerifier verifier(entry, trustedKeys);
    bool written = true;
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file) {
            result.diagnostic = {UpdateRefusal::FileUnreadable,
                                 "the download folder cannot be written"};
            return result;
        }
        TransportRequest request;
        request.url = entry.url;
        request.maximumBytes = entry.size;
        result.transport = transport.get(
            request,
            [&](const std::span<const std::byte> bytes) {
                file.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
                verifier.update(bytes);
                written = static_cast<bool>(file);
                return written;
            },
            cancelled, progress);
        file.flush();
        written = written && static_cast<bool>(file);
    }
    if (result.transport.status == TransportStatus::Cancelled) {
        discard();
        result.diagnostic = {UpdateRefusal::Cancelled, "the download was cancelled"};
        return result;
    }
    if (!written) {
        discard();
        result.transport.status = TransportStatus::Failed;
        result.diagnostic = {UpdateRefusal::FileUnreadable,
                             "the download could not be written to disk"};
        return result;
    }
    if (result.transport.status == TransportStatus::TooLarge) {
        discard();
        result.diagnostic = {UpdateRefusal::FileSizeMismatch,
                             "the server sent more than the feed gives for the file"};
        return result;
    }
    if (result.transport.status != TransportStatus::Ok) {
        discard();
        return result;
    }
    result.diagnostic = verifier.finish();
    if (!result.diagnostic.accepted()) {
        discard();
        return result;
    }
    std::filesystem::rename(partial, destination, error);
    if (error) {
        discard();
        result.diagnostic = {UpdateRefusal::FileUnreadable,
                             "the downloaded file could not be put in place"};
        return result;
    }
    result.file = destination;
    return result;
}

} // namespace bloom::update
