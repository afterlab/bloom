#include "settled_file.hpp"
#include "unique_temp_dir.hpp"
#include <bloom/platform/source_identity.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {
class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};
// Every fixture this test writes is settled (settled_file.hpp): the counts below are identity-only
// checks, which an unsettled identity never allows. The unsettled case has its own block below.
void write(const std::filesystem::path& path, const std::string& bytes) {
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }
    if (!bloom::test_support::settleFileIdentity(path))
        std::cerr << "FAILED: the fixture's change time could not be set back\n";
}
bloom::core::Sha256Digest digestOf(const std::string& bytes) {
    return *bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(bytes.data(), bytes.size())));
}
} // namespace

int main() {
    using namespace bloom::platform;
    Expectations expectations;
    const auto directory =
        bloom::test_support::processTempDirectory() /
        ("bloom-source-identity-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto source = directory / "clip.bin";
    const std::string original(200000, 'a');
    write(source, original);
    const auto expected = digestOf(original);

    const auto identity = observeFileIdentity(source);
    expectations.expect(identity && identity->size == original.size() && identity->fileIdLow != 0,
                        "a regular file has an identity");
    expectations.expect(!observeFileIdentity(directory), "a directory has no file identity");
    expectations.expect(!observeFileIdentity(directory / "missing"), "missing has no identity");

    // The first verification hashes once; repeated checks are identity-only.
    auto before = sourceReadWork();
    expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Matches,
                        "first verification matches");
    auto after = sourceReadWork();
    expectations.expect(after.fullReads == before.fullReads + 1 &&
                            after.fullReadBytes == before.fullReadBytes + original.size(),
                        "first verification is one counted full read");
    before = after;
    for (int i = 0; i < 100; ++i)
        expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Matches,
                            "repeated verification matches");
    after = sourceReadWork();
    expectations.expect(after.fullReads == before.fullReads, "repeated checks read nothing");
    expectations.expect(after.identityHits >= before.identityHits + 100,
                        "repeated checks are identity hits");

    // A same-size content rewrite changes the identity (mtime/ctime) and is detected by a rehash.
    std::string rewritten = original;
    rewritten[1000] = 'b';
    write(source, rewritten);
    before = sourceReadWork();
    expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Changed,
                        "a same-size rewrite reports Changed");
    after = sourceReadWork();
    expectations.expect(after.fullReads == before.fullReads + 1, "a changed identity rehashes");
    expectations.expect(verifySourceDigest(source, digestOf(rewritten)) == SourceCheck::Matches,
                        "the new content verifies without another read");
    expectations.expect(sourceReadWork().fullReads == after.fullReads,
                        "the rehash recorded the new identity");

    // A pure timestamp touch rehashes once and stays verified.
    const auto touched = std::filesystem::last_write_time(source) + std::chrono::seconds(3);
    std::filesystem::last_write_time(source, touched);
    expectations.expect(bloom::test_support::settleFileIdentity(source),
                        "the touched fixture is settled");
    before = sourceReadWork();
    expectations.expect(verifySourceDigest(source, digestOf(rewritten)) == SourceCheck::Matches,
                        "a touched file still matches");
    expectations.expect(sourceReadWork().fullReads == before.fullReads + 1,
                        "a touched file is rehashed once");

    // Replacing the path with a different file (new inode) is never a memo hit.
    const auto replacement = directory / "replacement.bin";
    write(replacement, rewritten);
    std::filesystem::rename(replacement, source);
    expectations.expect(!observeSource(source).digest, "a replaced inode is not a memo hit");

    std::filesystem::remove(source);
    expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Missing,
                        "a missing file reports Missing");
    expectations.expect(digestSourceFile(source).error == SourceHashError::Missing,
                        "digest of a missing file reports Missing");
    write(source, original);
    expectations.expect(hashSourceFile(source, {}, 10).error == SourceHashError::Oversized,
                        "the size limit refuses before reading");
    expectations.expect(hashSourceFile(source, [] { return true; }).error ==
                            SourceHashError::Cancelled,
                        "cancellation is reported");
    // #718: verified identities persist, so a source hashed in an earlier run is not read again.
    {
        auto& memo = verifiedSourceDigests();
        memo.clear();
        const auto file = directory / "cache" / "verified-sources.v1";
        // A name with the characters the file format escapes (Windows names hold no line feed).
#if defined(_WIN32)
        const auto odd = directory / "50% of a clip.bin";
#else
        const auto odd = directory / "50% of a\nclip.bin";
#endif
        write(odd, original);
        memo.persistTo(file);
        memo.load();
        before = sourceReadWork();
        expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Matches &&
                                verifySourceDigest(odd, expected) == SourceCheck::Matches &&
                                sourceReadWork().fullReads == before.fullReads + 2,
                            "the first run hashes each source once");
        expectations.expect(memo.persistedSize() == 2, "each verified source is on record");
        memo.save();
        expectations.expect(std::filesystem::is_regular_file(file),
                            "the verified identities are saved");
        // A new run: the memo starts empty and reads the file.
        memo.persistTo({});
        memo.clear();
        expectations.expect(!observeSource(source).digest, "a run without the file knows nothing");
        memo.persistTo(file);
        memo.load();
        before = sourceReadWork();
        expectations.expect(memo.persistedSize() == 2 &&
                                verifySourceDigest(source, expected) == SourceCheck::Matches &&
                                verifySourceDigest(odd, expected) == SourceCheck::Matches &&
                                sourceReadWork().fullReads == before.fullReads,
                            "the next run verifies both sources without reading them");
        // The volume id is not part of what persists: a share mounted again is the same file.
        auto remounted = *observeFileIdentity(source);
        remounted.volume += 7;
        memo.persistTo({});
        memo.clear();
        memo.persistTo(file);
        memo.load();
        expectations.expect(memo.find(source, remounted) == expected,
                            "a persisted identity stands under another volume id");
        const auto known = memo.knownIdentity(source, &remounted);
        expectations.expect(known && *known == remounted,
                            "the known identity of a persisted file is the current one");
        // A changed file is hashed again, and the new digest replaces the old record.
        write(source, rewritten);
        before = sourceReadWork();
        expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Changed &&
                                sourceReadWork().fullReads == before.fullReads + 1,
                            "a changed file is read and reported, once");
        memo.save();
        memo.persistTo({});
        memo.clear();
        memo.persistTo(file);
        memo.load();
        before = sourceReadWork();
        expectations.expect(verifySourceDigest(source, digestOf(rewritten)) ==
                                    SourceCheck::Matches &&
                                sourceReadWork().fullReads == before.fullReads,
                            "the changed file's new digest persists");
        // A forgotten source does not come back from the file, and a damaged file is ignored.
        memo.forget(odd);
        memo.save();
        memo.persistTo({});
        memo.clear();
        memo.persistTo(file);
        memo.load();
        expectations.expect(memo.persistedSize() == 1 && !observeSource(odd).digest,
                            "a forgotten source is not on record in the next run");
        write(file, "not a verified-sources file\n1 2 3\n");
        memo.persistTo(file);
        memo.load();
        expectations.expect(memo.persistedSize() == 0, "an unreadable record is ignored");
        // A lookup made before the record has been read waits for it instead of hashing.
        memo.clear();
        expectations.expect(verifySourceDigest(source, digestOf(rewritten)) ==
                                    SourceCheck::Matches &&
                                memo.persistedSize() == 1,
                            "the source is hashed again for the record");
        memo.save();
        memo.persistTo({});
        memo.clear();
        memo.persistTo(file);
        before = sourceReadWork();
        // No clock decides this: the wait is made as long as the test, and the record is read
        // only once the lookup is seen waiting.
        memo.setPersistedLoadWaitForTest(24 * 60 * 60 * 1000);
        std::optional<bloom::core::Sha256Digest> waited;
        std::thread lookup([&] { waited = observeSource(source).digest; });
        while (memo.persistedLoadWaitersForTest() == 0)
            std::this_thread::yield();
        memo.load();
        lookup.join();
        memo.setPersistedLoadWaitForTest(VerifiedSourceDigests::kPersistedLoadWaitMilliseconds);
        expectations.expect(waited == digestOf(rewritten) &&
                                sourceReadWork().fullReads == before.fullReads,
                            "a lookup that arrives before the record is read waits for it");
        memo.persistTo({});
        memo.clear();
    }
    // #749: an identity that is not settled (observed within the settle time of its last change on
    // a volume with no change journal) cannot tell a rewrite inside one clock tick from no change.
    // It vouches for nothing, and nothing recorded under it is put on record for the next run.
    {
        auto& memo = verifiedSourceDigests();
        memo.persistTo({});
        memo.clear();
        const auto racy = directory / "racy.bin";
        write(racy, original);
        const auto observed = observeFileIdentity(racy);
        expectations.expect(observed && observed->settled, "a settled fixture's identity is settled");
        if (observed) {
            auto unsettled = *observed;
            unsettled.settled = false;
            expectations.expect(unsettled == *observed,
                                "settledness is not part of what makes two identities equal");
            const auto record = directory / "racy-cache" / "verified-sources.v1";
            memo.persistTo(record);
            memo.load();
            memo.record(racy, unsettled, expected);
            expectations.expect(!memo.find(racy, unsettled) && !memo.find(racy, *observed),
                                "a digest recorded under an unsettled identity vouches for nothing");
            expectations.expect(memo.persistedSize() == 0,
                                "a digest recorded under an unsettled identity is not on record");
            memo.record(racy, *observed, expected);
            expectations.expect(memo.find(racy, *observed) == expected && memo.persistedSize() == 1,
                                "the same digest under a settled identity vouches and is on record");
            expectations.expect(!memo.find(racy, unsettled),
                                "an unsettled observation is never vouched for");
            memo.persistTo({});
            memo.clear();
        }
    }
    std::filesystem::remove_all(directory);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
