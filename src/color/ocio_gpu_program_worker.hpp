#pragma once

// Host side of the isolated FileTransform GPU-program extraction. It never parses LUT bytes: the
// caller passes the already-read, digest-verified bytes, which are streamed to bloom-color-worker
// as a sealed anonymous resource. The helper rebuilds the OCIO FileTransform processor, reflects
// the GPU program, and writes its bounded serialization into a second sealed resource; this
// function returns those exact bytes and a typed LutError. A helper that cannot be started or
// confined is HelperUnavailable; the bytes are never parsed in-process instead.

#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bloom::color::detail {

// Where bloom-color-worker is: installed beside or below this program, else where the build put
// it. Empty when neither exists.
[[nodiscard]] std::string lutWorkerPath();

struct FileTransformGpuExtraction final {
    std::vector<std::byte> bytes;
    LutError error = LutError::None;
};

[[nodiscard]] FileTransformGpuExtraction
extractFileTransformGpuProgram(std::span<const std::byte> lutBytes,
                               const core::Sha256Digest& expectedDigest, std::uint32_t lutFormat,
                               LutInterpolation interpolation, LutDirection direction,
                               const std::function<bool()>& cancellation);

} // namespace bloom::color::detail
