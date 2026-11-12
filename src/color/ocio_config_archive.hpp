#pragma once

// Reads a config archive (.ocioz, a zip) from memory into the same resource table a loose config
// is served from, so the helper opens an archive without a path: on macOS and Windows the confined
// helper can open no file, and on every system an archive then behaves exactly as a loose config
// does (docs/architecture/color-management.md, "The config request group as implemented").
//
// The reader is deliberately small and strict. It accepts stored and deflated entries of a plain
// (not zip64, not encrypted, single-disk) archive, rejects a name that is not a canonical
// root-relative key, a duplicate name, a symbolic-link entry, a size that disagrees with the
// stream or the CRC, and any entry or total beyond the loose-config limits.

#include "ocio_config_protocol.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace bloom::color::detail {

struct ConfigArchiveTable final {
    // The extracted bytes of every file, back to back.
    std::vector<std::byte> payload;
    // Strictly ordered by key; offsets index into `payload`.
    std::vector<ConfigResourceSlice> index;
};

// Empty when the archive is not acceptable. Throws std::bad_alloc only.
[[nodiscard]] std::optional<ConfigArchiveTable> readConfigArchive(std::span<const std::byte> bytes);

} // namespace bloom::color::detail
