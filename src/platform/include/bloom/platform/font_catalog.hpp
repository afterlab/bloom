#pragma once

#include <bloom/core/sha256.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::platform {

enum class FontCatalogueStatus : std::uint8_t { Available, Unavailable };
enum class FontSource : std::uint8_t { Embedded, System };

struct FontFace final {
    std::string family;
    std::string style;
    std::int32_t weight = 400;
    std::int32_t width = 100;
    bool italic = false;
    std::filesystem::path path;
    core::Sha256Digest contentDigest;
    FontSource source = FontSource::System;
    std::uint32_t faceIndex = 0;
    // The face's PostScript name when the platform reports one: the most exact way to find an
    // installed font again (a font ID stores it).
    std::string postscriptName;

    friend bool operator==(const FontFace&, const FontFace&) = default;
};

struct FontCatalogue final {
    FontCatalogueStatus status = FontCatalogueStatus::Unavailable;
    std::vector<FontFace> faces;

    [[nodiscard]] bool available() const noexcept {
        return status == FontCatalogueStatus::Available;
    }
    [[nodiscard]] const FontFace* findByDigest(const core::Sha256Digest& digest) const noexcept;
};

class FontCatalogueProvider final {
  public:
    using Cancellation = std::function<bool()>;

    // Returns the bundled faces without consulting the operating system. UI catalogues use this
    // small immediate snapshot while the complete system enumeration runs on a worker.
    [[nodiscard]] static FontCatalogue embeddedCatalogue();

    // Enumeration is deliberately synchronous at this low-level boundary. Callers use it from a
    // worker (FontCatalogueCache is the host convenience wrapper); no fontconfig or filesystem
    // work is ever needed by the render evaluator.
    //
    // `digests` false skips hashing every font file: a lookup by name needs no content identity,
    // and hashing a few hundred installed fonts is the slow part of a scan. The faces then carry an
    // empty digest.
    [[nodiscard]] FontCatalogue enumerate(const Cancellation& cancelled = {},
                                          bool digests = true) const;

    // Faces read directly from the font files below `directories` (sfnt name and OS/2 tables),
    // without a platform font database. The Windows catalogue names the files DirectWrite lists
    // with the same reader; it is a public function so that reader is tested on every platform.
    [[nodiscard]] static FontCatalogue
    enumerateDirectories(std::span<const std::filesystem::path> directories,
                         const Cancellation& cancelled = {}, bool digests = true);

    // The SHA-256 of a font file, or nullopt when it is missing, empty or over 64 MiB.
    [[nodiscard]] static std::optional<core::Sha256Digest>
    digestFile(const std::filesystem::path& path);
};

// The names and style of every face in one font file's bytes (a single font or a collection).
struct SfntFaceNames final {
    std::string family;
    std::string style;
    std::string postscriptName;
    std::int32_t weight = 400;
    bool italic = false;
    std::uint32_t faceIndex = 0;
};
[[nodiscard]] std::vector<SfntFaceNames> readSfntFaceNames(std::span<const std::uint8_t> bytes);

} // namespace bloom::platform
