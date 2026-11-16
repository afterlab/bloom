// The AppImage's delta update: Bloom's control-file writer against the reference tool's file,
// the reader, finding a new file's blocks inside an older file, and the download that takes what
// it can from the installed file, asks for the rest in ranges, and falls back to the whole file
// whenever anything about the delta is wrong.
#include "update_test_support.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/update/base64.hpp>
#include <bloom/update/keys.hpp>
#include <bloom/update/zsync.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;
using std::filesystem::path;

// The time in the reference file's MTime line (Mon, 16 Nov 2026 21:27:27 +0000).
constexpr std::int64_t kReferenceTime = 1794864447;
constexpr std::string_view kTargetUrl =
    "https://github.com/afterlab/bloom/releases/download/v9/Bloom-new.AppImage";
constexpr std::string_view kControlUrl =
    "https://github.com/afterlab/bloom/releases/download/v9/Bloom-new.AppImage.zsync";

// The release file of reference/README.md: 9375 SHA-256 digests, 300,000 bytes.
[[nodiscard]] std::string newFile() {
    std::string data;
    data.reserve(300'000);
    for (int index = 0; index < 9375; ++index) {
        const auto text = "bloom zsync fixture " + std::to_string(index);
        const auto digest = bloom::core::Sha256Hasher::hash(asBytes(text));
        for (const std::uint8_t byte : digest->bytes()) {
            data.push_back(static_cast<char>(byte));
        }
    }
    return data;
}

// An older version of it: other bytes in front, the first third, a part that changed, and the
// rest from the middle on. Nothing after the end.
[[nodiscard]] std::string oldFile(const std::string& released) {
    std::string filler;
    for (int index = 0; filler.size() < 60'000; ++index) {
        const auto text = "an older version " + std::to_string(index);
        const auto digest = bloom::core::Sha256Hasher::hash(asBytes(text));
        for (const std::uint8_t byte : digest->bytes()) {
            filler.push_back(static_cast<char>(byte));
        }
    }
    return filler.substr(0, 1000) + released.substr(0, 100'000) + filler.substr(1000, 50'000) +
           released.substr(150'000);
}

void write(const path& file, const std::string_view content) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(content.data(),
                                                static_cast<std::streamsize>(content.size()));
}

[[nodiscard]] std::string read(const path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string referenceControl() {
    return read(path(BLOOM_UPDATE_FIXTURES).parent_path() / "reference" / "zsync-0.6.6.zsync");
}

// An entry as a verified feed would give it, from the fixture's checksum and signature.
[[nodiscard]] FeedFile entry(const std::string_view kind, const std::string_view url,
                             const std::string& content, const std::string& fixture) {
    FeedFile file;
    file.kind = std::string(kind);
    file.url = std::string(url);
    file.size = content.size();
    file.sha256 = *bloom::core::Sha256Hasher::hash(asBytes(content));
    const auto signature = decodeBase64(trimmedFixture(fixture + ".sig"));
    if (signature.has_value() && signature->size() == file.signature.size()) {
        std::ranges::copy(*signature, file.signature.begin());
    }
    return file;
}

// A server in memory: files by address, whole or in ranges.
class FakeTransport final : public UpdateTransport {
  public:
    std::map<std::string, std::string, std::less<>> files;
    bool givesRanges = true;
    // A range answer with one byte changed.
    bool damagesRanges = false;
    // Says "cancelled" from this request on (1 = the first).
    int cancelFromRequest = 0;

    int requests = 0;
    int rangeRequests = 0;
    int wholeRequestsFor(const std::string_view url) const {
        const auto found = whole_.find(url);
        return found == whole_.end() ? 0 : found->second;
    }
    std::uint64_t rangeBytes = 0;

    [[nodiscard]] TransportResult get(const TransportRequest& request, const TransportSink& sink,
                                      const std::function<bool()>& /*cancelled*/,
                                      const TransportProgress& /*progress*/) override {
        ++requests;
        TransportResult result;
        if (cancelFromRequest != 0 && requests >= cancelFromRequest) {
            result.status = TransportStatus::Cancelled;
            return result;
        }
        const auto found = files.find(request.url);
        if (found == files.end()) {
            result.status = TransportStatus::HttpError;
            result.httpStatus = 404;
            return result;
        }
        std::string body = found->second;
        if (request.range.has_value() && givesRanges) {
            ++rangeRequests;
            body = body.substr(static_cast<std::size_t>(request.range->first),
                               static_cast<std::size_t>(request.range->last -
                                                        request.range->first + 1));
            rangeBytes += body.size();
            if (damagesRanges && !body.empty()) {
                body[body.size() / 2] = static_cast<char>(body[body.size() / 2] ^ 0x20);
            }
            result.partial = true;
            result.httpStatus = 206;
        } else {
            // A request for a part that is answered with everything is not a whole download.
            if (!request.range.has_value()) {
                ++whole_[request.url];
            }
            result.httpStatus = 200;
        }
        if (body.size() > request.maximumBytes) {
            result.status = TransportStatus::TooLarge;
            return result;
        }
        // In two pieces, as a transfer arrives.
        const auto half = body.size() / 2;
        const bool taken = sink(asBytes(std::string_view(body).substr(0, half))) &&
                           sink(asBytes(std::string_view(body).substr(half)));
        result.status = taken ? TransportStatus::Ok : TransportStatus::Failed;
        return result;
    }

  private:
    std::map<std::string, int, std::less<>> whole_;
};

} // namespace

int main() {
    Expectations checks;
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error) /
                      ("bloom-update-zsync-" + std::to_string(std::random_device{}()));
    const std::string released = newFile();
    const std::string older = oldFile(released);
    const std::string reference = referenceControl();
    const auto keys = parsePublicKeys(trimmedFixture("trusted-key.txt"));
    checks.expect(released.size() == 300'000 && !reference.empty() && keys.has_value(),
                  "the release file, the reference control file and the key are there");
    if (reference.empty() || !keys.has_value()) {
        return 1;
    }
    write(base / "new.bin", released);
    write(base / "old.bin", older);

    // The writer gives the reference tool's file, byte for byte.
    {
        const auto made =
            makeZsyncControl(base / "new.bin", {"Bloom-new.AppImage", kReferenceTime});
        checks.expect(made.has_value(), "writer: the control file is made");
        if (made.has_value()) {
            const std::string_view text(reinterpret_cast<const char*>(made->data()), made->size());
            checks.expect(text == reference,
                          "writer: it is the reference tool's control file, byte for byte");
            if (text != reference) {
                const auto end = text.find("\n\n");
                std::cerr << text.substr(0, end == std::string_view::npos ? 0 : end) << '\n';
            }
        }
        checks.expect(!makeZsyncControl(base / "no-such-file", {"x", 0}).has_value(),
                      "writer: a file that cannot be read gives nothing");
    }

    // The reader.
    const auto control = readZsyncControl(asBytes(reference));
    {
        checks.expect(control.has_value() && control->length == 300'000 &&
                          control->blockSize == 2048 && control->sequentialMatches == 1 &&
                          control->weakBytes == 3 && control->strongBytes == 6 &&
                          control->blocks.size() == 147,
                      "reader: the reference file's header and its 147 blocks are read");
        const auto refused = [&](std::string changed) {
            return !readZsyncControl(asBytes(changed)).has_value();
        };
        const auto replaced = [&](const std::string_view from, const std::string_view to) {
            std::string changed = reference;
            const auto at = changed.find(from);
            if (at != std::string::npos) {
                changed.replace(at, from.size(), to);
            }
            return changed;
        };
        checks.expect(refused(reference.substr(0, reference.size() - 1)) &&
                          refused(reference + "x"),
                      "reader: a block list that is too short or too long is refused");
        checks.expect(refused(replaced("Blocksize: 2048", "Blocksize: 2000")) &&
                          refused(replaced("Blocksize: 2048", "Blocksize: 0")) &&
                          refused(replaced("Blocksize: 2048", "Blocksize: 4294967296")),
                      "reader: a block size that is not a usable power of two is refused");
        checks.expect(refused(replaced("Hash-Lengths: 1,3,6", "Hash-Lengths: 1,9,6")) &&
                          refused(replaced("Hash-Lengths: 1,3,6", "Hash-Lengths: 1,3,40")) &&
                          refused(replaced("Hash-Lengths: 1,3,6", "Hash-Lengths: 1,3")),
                      "reader: checksum lengths outside the format are refused");
        checks.expect(refused(replaced("Length: 300000", "Length: 0")) &&
                          refused(replaced("Length: 300000", "Length: 99999999999999999")),
                      "reader: an empty or absurd length is refused");
        checks.expect(refused(replaced("URL: ", "Z-URL: ")) &&
                          refused(replaced("Strong-Hash-Algorithm: MD4", "Recompress: gzip -9")),
                      "reader: the compressed-stream additions are refused");
        checks.expect(refused(replaced("Safe: File-Hash,Strong-Hash-Algorithm\n", "")),
                      "reader: a line it does not know and that is not marked safe is refused");
        checks.expect(refused(replaced("\n\n", "\n")) && refused("") &&
                          refused(std::string(100, '\0')),
                      "reader: a file without a header is refused");
    }
    if (!control.has_value()) {
        return 1;
    }

    // Blocks of the new file are found inside the older one wherever they are.
    {
        const auto never = [] { return false; };
        const auto match = matchLocalBlocks(*control, base / "old.bin", never);
        checks.expect(match.has_value() && match->localOffsets.size() == 147,
                      "match: every block of the new file gets an answer");
        if (match.has_value() && match->localOffsets.size() == 147) {
            bool right = true;
            std::uint64_t reused = 0;
            for (std::uint64_t block = 0; block < 147; ++block) {
                const std::uint64_t start = block * 2048;
                const std::uint64_t end = start + 2048;
                std::int64_t expected = -1;
                if (end <= 100'000) {
                    expected = static_cast<std::int64_t>(1000 + start);
                } else if (start >= 150'000 && end <= 300'000) {
                    expected = static_cast<std::int64_t>(1000 + 100'000 + 50'000 +
                                                         (start - 150'000));
                }
                right = right && match->localOffsets[block] == expected;
                reused += expected >= 0 ? 2048 : 0;
            }
            checks.expect(right, "match: the unchanged blocks are found at their new places, the "
                                 "changed ones and the short last one are not");
            checks.expect(match->reusedBytes == reused && reused > 200'000,
                          "match: most of the file is already there");
        }
        const auto same = matchLocalBlocks(*control, base / "new.bin", never);
        checks.expect(same.has_value() &&
                          std::count(same->localOffsets.begin(), same->localOffsets.end(), -1) ==
                              1 &&
                          same->localOffsets.back() == -1,
                      "match: the same file has every block but the short last one");
        write(base / "short.bin", released.substr(0, 1000));
        const auto tiny = matchLocalBlocks(*control, base / "short.bin", never);
        checks.expect(tiny.has_value() && tiny->reusedBytes == 0,
                      "match: a file shorter than a block has none");
        checks.expect(!matchLocalBlocks(*control, base / "no-such-file", never).has_value(),
                      "match: a file that cannot be read gives nothing");
    }

    // The download.
    const FeedFile target = entry("appimage", kTargetUrl, released, "zsync-new.bin");
    const FeedFile controlEntry = entry("zsync", kControlUrl, reference, "zsync-control");
    const auto never = [] { return false; };
    const auto serve = [&](FakeTransport& transport) {
        transport.files[std::string(kTargetUrl)] = released;
        transport.files[std::string(kControlUrl)] = reference;
    };
    int scene = 0;
    const auto destination = [&] { return base / ("download-" + std::to_string(++scene)) / "new"; };
    const auto onlyFile = [](const path& file) {
        int entries = 0;
        std::error_code ignored;
        for (const auto& item : std::filesystem::directory_iterator(file.parent_path(), ignored)) {
            (void)item;
            ++entries;
        }
        return entries;
    };

    // The delta is used: two parts are fetched, the rest comes from the installed file.
    {
        FakeTransport transport;
        serve(transport);
        DeltaStatistics statistics;
        std::uint64_t reported = 0;
        std::uint64_t reportedTotal = 0;
        const auto file = destination();
        const auto result = downloadWithDelta(
            transport, target, controlEntry, base / "old.bin", *keys, file, never,
            [&](const std::uint64_t received, const std::uint64_t total) {
                reported = received;
                reportedTotal = total;
            },
            &statistics);
        checks.expect(result.succeeded() && result.file == file && read(file) == released,
                      "delta: the result is the release file");
        checks.expect(statistics.used && statistics.fallbackReason.empty() &&
                          statistics.reusedBytes + statistics.fetchedBytes == released.size() &&
                          statistics.fetchedBytes == transport.rangeBytes &&
                          statistics.fetchedBytes < released.size() / 3,
                      "delta: most of it came from the installed file");
        checks.expect(transport.rangeRequests == 2 &&
                          transport.wholeRequestsFor(kTargetUrl) == 0 &&
                          transport.wholeRequestsFor(kControlUrl) == 1,
                      "delta: the control file and two ranges were asked for, never the whole file");
        checks.expect(reported == released.size() && reportedTotal == released.size(),
                      "delta: progress ends at the file's size");
        checks.expect(onlyFile(file) == 1, "delta: nothing else is left beside the file");
    }
    // The installed file is the new one already: only the short last block is fetched.
    {
        FakeTransport transport;
        serve(transport);
        DeltaStatistics statistics;
        const auto file = destination();
        const auto result = downloadWithDelta(transport, target, controlEntry, base / "new.bin",
                                              *keys, file, never, {}, &statistics);
        checks.expect(result.succeeded() && read(file) == released && statistics.used &&
                          statistics.fetchedBytes == 300'000 % 2048 &&
                          transport.rangeRequests == 1,
                      "same: a file that is already there costs one small range");
    }
    // Every way the delta can fail ends in the whole file, verified.
    const auto fallsBack = [&](const std::string& name, const path& installed,
                               const std::function<void(FakeTransport&)>& arrange,
                               const FeedFile& controlGiven) {
        FakeTransport transport;
        serve(transport);
        arrange(transport);
        DeltaStatistics statistics;
        const auto file = destination();
        const auto result = downloadWithDelta(transport, target, controlGiven, installed, *keys,
                                              file, never, {}, &statistics);
        checks.expect(result.succeeded() && read(file) == released,
                      name + ": the whole file is downloaded and accepted");
        checks.expect(!statistics.used && !statistics.fallbackReason.empty() &&
                          transport.wholeRequestsFor(kTargetUrl) == 1,
                      name + ": the delta says why it was not used (" +
                          statistics.fallbackReason + ")");
        checks.expect(onlyFile(file) == 1, name + ": nothing else is left beside the file");
    };
    fallsBack(
        "no ranges", base / "old.bin", [](FakeTransport& transport) { transport.givesRanges = false; },
        controlEntry);
    fallsBack(
        "damaged range", base / "old.bin",
        [](FakeTransport& transport) { transport.damagesRanges = true; }, controlEntry);
    fallsBack(
        "changed control file", base / "old.bin",
        [&](FakeTransport& transport) {
            std::string changed = reference;
            changed[changed.size() - 3] = static_cast<char>(changed[changed.size() - 3] ^ 0x01);
            transport.files[std::string(kControlUrl)] = changed;
        },
        controlEntry);
    fallsBack(
        "no control file", base / "old.bin",
        [](FakeTransport& transport) { transport.files.erase(std::string(kControlUrl)); },
        controlEntry);
    fallsBack("no installed file", base / "no-such-file", [](FakeTransport&) {}, controlEntry);
    write(base / "other.bin", older.substr(1000 + 100'000, 50'000));
    fallsBack("nothing in common", base / "other.bin", [](FakeTransport&) {}, controlEntry);
    {
        // A control file for another file: signed and whole, but not this release file's.
        const std::string shorter = released.substr(0, 200'000);
        write(base / "shorter.bin", shorter);
        const auto made =
            makeZsyncControl(base / "shorter.bin", {"Bloom-new.AppImage", kReferenceTime});
        if (made.has_value()) {
            const std::string text(reinterpret_cast<const char*>(made->data()), made->size());
            FeedFile other = controlEntry;
            other.size = text.size();
            other.sha256 = *bloom::core::Sha256Hasher::hash(asBytes(text));
            // (Its signature is not this file's, so it is refused before it is read.)
            fallsBack(
                "another file's control file", base / "old.bin",
                [&](FakeTransport& transport) {
                    transport.files[std::string(kControlUrl)] = text;
                },
                other);
        }
    }
    // A whole file that is not the release file is refused, with or without the delta.
    {
        FakeTransport transport;
        serve(transport);
        transport.givesRanges = false;
        std::string tampered = released;
        tampered[1234] = static_cast<char>(tampered[1234] ^ 0x01);
        transport.files[std::string(kTargetUrl)] = tampered;
        const auto file = destination();
        const auto result = downloadWithDelta(transport, target, controlEntry, base / "old.bin",
                                              *keys, file, never, {});
        checks.expect(!result.succeeded() &&
                          result.diagnostic.refusal == UpdateRefusal::FileChecksumMismatch &&
                          onlyFile(file) == 0,
                      "tampered: a changed release file is refused and nothing is kept");
    }
    // Cancelled while the parts are fetched: nothing is kept and nothing more is asked for.
    {
        FakeTransport transport;
        serve(transport);
        transport.cancelFromRequest = 2;
        bool cancel = false;
        const auto file = destination();
        const auto result = downloadWithDelta(
            transport, target, controlEntry, base / "old.bin", *keys, file,
            [&] { return cancel; },
            [&](std::uint64_t, std::uint64_t) { cancel = true; });
        checks.expect(!result.succeeded() &&
                          result.diagnostic.refusal == UpdateRefusal::Cancelled &&
                          transport.wholeRequestsFor(kTargetUrl) == 0 && onlyFile(file) == 0,
                      "cancelled: the download stops, keeps nothing and does not start again");
    }

    std::filesystem::remove_all(base, error);
    return checks.result();
}
