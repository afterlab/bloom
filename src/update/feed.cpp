#include <bloom/update/feed.hpp>

#include <bloom/update/base64.hpp>
#include <bloom/update/keys.hpp>

#include <yyjson.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace bloom::update {
namespace {

struct DocumentDeleter final {
    void operator()(yyjson_doc* document) const noexcept { yyjson_doc_free(document); }
};
using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;

[[nodiscard]] Document parseJson(const std::span<const std::uint8_t> bytes) {
    // yyjson reads the buffer without changing it under these flags.
    return Document(yyjson_read_opts(
        const_cast<char*>(reinterpret_cast<const char*>(bytes.data())), bytes.size(),
        YYJSON_READ_NOFLAG, nullptr, nullptr));
}

[[nodiscard]] std::optional<std::string_view> stringOf(yyjson_val* value) noexcept {
    if (!yyjson_is_str(value)) {
        return std::nullopt;
    }
    return std::string_view(yyjson_get_str(value), yyjson_get_len(value));
}

[[nodiscard]] std::optional<std::string_view> memberString(yyjson_val* object,
                                                           const char* name) noexcept {
    return stringOf(yyjson_obj_get(object, name));
}

[[nodiscard]] FeedReadResult refused(const UpdateRefusal refusal, std::string detail) {
    return {std::nullopt, {refusal, std::move(detail)}};
}

[[nodiscard]] bool isPlainText(const std::string_view text) noexcept {
    return std::ranges::all_of(text, [](const char character) {
        return static_cast<unsigned char>(character) > 0x20 &&
               static_cast<unsigned char>(character) < 0x7f;
    });
}

[[nodiscard]] bool isDate(const std::string_view text) noexcept {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isKindName(const std::string_view text) noexcept {
    return !text.empty() && text.size() <= 32 &&
           std::ranges::all_of(text, [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '-';
           });
}

// The host of an http(s) address, without user information or port; empty when there is none.
[[nodiscard]] std::string_view hostOf(std::string_view rest) noexcept {
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (rest.find('@') != std::string_view::npos) {
        return {};
    }
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        return close == std::string_view::npos ? std::string_view{} : rest.substr(0, close + 1);
    }
    return rest.substr(0, rest.find(':'));
}

struct FileReadResult final {
    std::optional<FeedFile> file;
    UpdateRefusal refusal = UpdateRefusal::FeedContentMalformed;
    std::string detail;
};

[[nodiscard]] FileReadResult readFile(yyjson_val* value, const FeedReadOptions& options) {
    if (!yyjson_is_obj(value)) {
        return {std::nullopt, UpdateRefusal::FeedContentMalformed, "a file entry is not an object"};
    }
    FeedFile file;
    const auto kind = memberString(value, "kind");
    const auto url = memberString(value, "url");
    const auto sha256 = memberString(value, "sha256");
    const auto signature = memberString(value, "signature");
    yyjson_val* size = yyjson_obj_get(value, "size");
    if (!kind.has_value() || !isKindName(*kind) || !url.has_value() || !sha256.has_value() ||
        !signature.has_value() || !yyjson_is_uint(size) || yyjson_get_uint(size) == 0) {
        return {std::nullopt, UpdateRefusal::FeedContentMalformed,
                "a file entry lacks its kind, address, size, checksum or signature"};
    }
    if (!updateAddressAllowed(*url, options.allowLoopbackHttp)) {
        return {std::nullopt, UpdateRefusal::FeedAddressRefused,
                "the address of a file is not an HTTPS address"};
    }
    const auto digest = core::Sha256Digest::fromLowercaseHex(*sha256);
    const auto signatureBytes = decodeBase64(*signature);
    if (!digest.has_value() || !signatureBytes.has_value() ||
        signatureBytes->size() != kEd25519SignatureBytes) {
        return {std::nullopt, UpdateRefusal::FeedContentMalformed,
                "a file entry's checksum or signature is not in its written form"};
    }
    file.kind = std::string(*kind);
    file.url = std::string(*url);
    file.size = yyjson_get_uint(size);
    file.sha256 = *digest;
    std::ranges::copy(*signatureBytes, file.signature.begin());
    return {std::move(file), UpdateRefusal::None, {}};
}

[[nodiscard]] FeedReadResult readContent(const std::span<const std::uint8_t> payload,
                                         const FeedReadOptions& options) {
    const Document document = parseJson(payload);
    yyjson_val* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
    if (!yyjson_is_obj(root)) {
        return refused(UpdateRefusal::FeedContentMalformed, "the signed content is not a JSON object");
    }
    if (memberString(root, "schema") != kFeedSchema) {
        return refused(UpdateRefusal::FeedContentMalformed,
                       "the signed content is not an update feed of a known form");
    }
    const auto channel = memberString(root, "channel");
    const auto versionText = memberString(root, "version");
    const auto published = memberString(root, "published");
    const auto notes = memberString(root, "notes");
    yyjson_val* platforms = yyjson_obj_get(root, "platforms");
    if (!channel.has_value() || !versionText.has_value() || !published.has_value() ||
        !notes.has_value() || !yyjson_is_obj(platforms) || !isDate(*published)) {
        return refused(UpdateRefusal::FeedContentMalformed,
                       "the feed lacks its channel, version, date, notes or platforms");
    }
    auto version = ReleaseVersion::parse(*versionText);
    if (!version.has_value()) {
        return refused(UpdateRefusal::FeedContentMalformed, "the feed's version is not a version");
    }
    if (*channel != options.channel) {
        return refused(UpdateRefusal::FeedChannelMismatch,
                       "the feed is for the '" + std::string(*channel) + "' channel");
    }
    if (version->channel() != *channel) {
        return refused(UpdateRefusal::FeedContentMalformed,
                       "the feed's version does not belong to its channel");
    }

    UpdateFeed feed;
    feed.channel = std::string(*channel);
    feed.version = std::move(*version);
    feed.published = std::string(*published);
    feed.notes = std::string(*notes);

    yyjson_obj_iter iterator = yyjson_obj_iter_with(platforms);
    while (yyjson_val* key = yyjson_obj_iter_next(&iterator)) {
        yyjson_val* value = yyjson_obj_iter_get_val(key);
        const auto name = stringOf(key);
        yyjson_val* files = yyjson_obj_get(value, "files");
        if (!name.has_value() || name->empty() || !isPlainText(*name) || !yyjson_is_obj(value) ||
            !yyjson_is_arr(files) || yyjson_arr_size(files) == 0) {
            return refused(UpdateRefusal::FeedContentMalformed,
                           "a platform of the feed is not an object with files");
        }
        FeedPlatform platform;
        if (yyjson_val* appcast = yyjson_obj_get(value, "appcast"); appcast != nullptr) {
            const auto address = stringOf(appcast);
            if (!address.has_value()) {
                return refused(UpdateRefusal::FeedContentMalformed,
                               "a platform's updater address is not text");
            }
            if (!updateAddressAllowed(*address, options.allowLoopbackHttp)) {
                return refused(UpdateRefusal::FeedAddressRefused,
                               "a platform's updater address is not an HTTPS address");
            }
            platform.appcastUrl = std::string(*address);
        }
        yyjson_arr_iter fileIterator = yyjson_arr_iter_with(files);
        while (yyjson_val* entry = yyjson_arr_iter_next(&fileIterator)) {
            auto file = readFile(entry, options);
            if (!file.file.has_value()) {
                return refused(file.refusal, std::move(file.detail));
            }
            platform.files.push_back(std::move(*file.file));
        }
        if (!feed.platforms.emplace(std::string(*name), std::move(platform)).second) {
            return refused(UpdateRefusal::FeedContentMalformed, "the feed names a platform twice");
        }
    }
    return {std::move(feed), {}};
}

} // namespace

const FeedFile* FeedPlatform::file(const std::string_view kind) const noexcept {
    const auto found =
        std::ranges::find_if(files, [kind](const FeedFile& entry) { return entry.kind == kind; });
    return found == files.end() ? nullptr : &*found;
}

const FeedPlatform* UpdateFeed::platform(const std::string_view key) const noexcept {
    const auto found = platforms.find(key);
    return found == platforms.end() ? nullptr : &found->second;
}

std::vector<std::byte> feedSignedBytes(const std::string_view payloadType,
                                       const std::span<const std::uint8_t> payload) {
    std::string header = "DSSEv1 " + std::to_string(payloadType.size()) + ' ';
    header += payloadType;
    header += ' ' + std::to_string(payload.size()) + ' ';
    std::vector<std::byte> bytes(header.size() + payload.size());
    std::memcpy(bytes.data(), header.data(), header.size());
    if (!payload.empty()) {
        std::memcpy(bytes.data() + header.size(), payload.data(), payload.size());
    }
    return bytes;
}

bool updateAddressAllowed(const std::string_view url, const bool allowLoopbackHttp) noexcept {
    if (url.size() > 2048 || !isPlainText(url)) {
        return false;
    }
    constexpr std::string_view secure = "https://";
    constexpr std::string_view plain = "http://";
    if (url.starts_with(secure)) {
        return !hostOf(url.substr(secure.size())).empty();
    }
    if (allowLoopbackHttp && url.starts_with(plain)) {
        const auto host = hostOf(url.substr(plain.size()));
        return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
    }
    return false;
}

std::string_view currentPlatformKey() noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(_WIN32) && defined(_M_X64)
    return "windows-x86_64";
#else
    return {};
#endif
}

FeedReadResult readSignedFeed(const std::span<const std::byte> bytes,
                              const FeedReadOptions& options) {
    if (options.trustedKeys.empty()) {
        return refused(UpdateRefusal::NotConfigured, "this build carries no update key");
    }
    if (bytes.size() > kMaximumFeedBytes) {
        return refused(UpdateRefusal::FeedTooLarge, "the feed is larger than a feed can be");
    }
    const Document envelope = parseJson(
        std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    yyjson_val* root = envelope ? yyjson_doc_get_root(envelope.get()) : nullptr;
    if (!yyjson_is_obj(root)) {
        return refused(UpdateRefusal::FeedMalformed, "the feed is not a JSON object");
    }
    const auto payloadType = memberString(root, "payloadType");
    const auto payloadText = memberString(root, "payload");
    yyjson_val* signatures = yyjson_obj_get(root, "signatures");
    if (!payloadType.has_value() || !payloadText.has_value() || !yyjson_is_arr(signatures)) {
        return refused(UpdateRefusal::FeedMalformed, "the feed is not a signed envelope");
    }
    if (*payloadType != kFeedPayloadType) {
        return refused(UpdateRefusal::FeedMalformed, "the envelope does not hold an update feed");
    }
    const auto payload = decodeBase64(*payloadText);
    if (!payload.has_value()) {
        return refused(UpdateRefusal::FeedMalformed, "the envelope's content is not Base64");
    }
    if (yyjson_arr_size(signatures) == 0) {
        return refused(UpdateRefusal::FeedUnsigned, "the feed carries no signature");
    }

    // One signature by a key this build carries is enough; a signature by another key says
    // nothing either way (a release may be signed by a new and an old key while one replaces
    // the other). A signature that names a carried key and does not match is a refusal.
    if (!cryptoLibraryReady()) {
        return refused(UpdateRefusal::VerificationUnavailable,
                       "this copy of Bloom cannot check signatures: " + cryptoLibraryProblem());
    }
    const auto signedBytes = feedSignedBytes(*payloadType, *payload);
    bool verified = false;
    bool mismatch = false;
    yyjson_arr_iter iterator = yyjson_arr_iter_with(signatures);
    while (yyjson_val* entry = yyjson_arr_iter_next(&iterator)) {
        const auto keyId = memberString(entry, "keyid");
        const auto signatureText = memberString(entry, "sig");
        if (!yyjson_is_obj(entry) || !keyId.has_value() || !signatureText.has_value()) {
            return refused(UpdateRefusal::FeedMalformed, "a signature of the feed is malformed");
        }
        const auto signatureBytes = decodeBase64(*signatureText);
        if (!signatureBytes.has_value() || signatureBytes->size() != kEd25519SignatureBytes) {
            return refused(UpdateRefusal::FeedMalformed, "a signature of the feed is malformed");
        }
        Ed25519Signature signature{};
        std::ranges::copy(*signatureBytes, signature.begin());
        for (const auto& key : options.trustedKeys) {
            if (publicKeyId(key) != *keyId) {
                continue;
            }
            if (verifyEd25519(key, signedBytes, signature)) {
                verified = true;
            } else {
                mismatch = true;
            }
        }
    }
    if (mismatch) {
        return refused(UpdateRefusal::FeedSignatureInvalid,
                       "the feed's signature does not match its content");
    }
    if (!verified) {
        return refused(UpdateRefusal::FeedUnknownKey,
                       "the feed is not signed by a key this build carries");
    }
    return readContent(*payload, options);
}

} // namespace bloom::update
