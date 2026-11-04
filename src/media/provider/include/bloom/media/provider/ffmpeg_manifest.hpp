#pragma once
#include <bloom/media/provider/protocol.hpp>
namespace bloom::media::provider {
// Codec-free declaration shared by the host and the isolated executable.
// `hardware` selects the VA-API encode generation; the software generation always declares the
// bundled OpenH264 (H.264), Kvazaar (HEVC), libvpx (VP9), SVT-AV1 (AV1) and libopus (Opus)
// encoders.
[[nodiscard]] Handshake ffmpegHandshake(bool hardware = false);
// The NVIDIA NVENC encode generation (`--nvenc`): H.264 High and HEVC Main/Main 10 through
// h264_nvenc/hevc_nvenc, with the driver loaded at run time. Hardware execution key, no
// determinism claim; every export still passes the reopen check.
[[nodiscard]] Handshake ffmpegNvencHandshake();
// Hardware decode generations for H.264, HEVC and AV1 (ADR 0027, amended 2026-11-04): VA-API
// with run-time render-node discovery (`--vaapi-decode`), or NVIDIA NVDEC through FFmpeg's CUDA
// hwaccel (`--nvdec-decode`). Each worker generation proves its first hardware frame byte-identical
// to the software reference; the host keeps the explicit, permanent per-source software fallback.
// Software decode remains the reference. Preview and final rendering may both use a proven source.
enum class HardwareDecodeDevice : std::uint8_t { Vaapi, Nvdec };
[[nodiscard]] Handshake
ffmpegHardwareDecodeHandshake(HardwareDecodeDevice device = HardwareDecodeDevice::Vaapi);
[[nodiscard]] PipelineQualificationV1 ffmpegPipeline(const ProviderDeclaration& declaration);
// Presentation notes for a ProRes deliverable, by the implementation that served it. On macOS
// ProRes is the operating system's own codec through AVFoundation/VideoToolbox -- the native,
// Apple-authorized implementation -- so the FFmpeg non-authorization caveat does not apply to it.
inline constexpr const char* kProResVideoToolboxPreviewNote =
    "Apple ProRes decoded by AVFoundation/VideoToolbox";
inline constexpr const char* kProResVideoToolboxExportNote =
    "Apple ProRes encoded by AVFoundation/VideoToolbox; native Apple implementation";
inline constexpr const char* kProResFfmpegPreviewNote =
    "Decoded by FFmpeg; not an Apple-authorized ProRes implementation";
inline constexpr const char* kProResFfmpegExportNote =
    "Decoded/encoded by FFmpeg; not an Apple-authorized ProRes implementation";
// The note for this platform's preferred provider (VideoToolbox first on macOS).
#if defined(__APPLE__)
inline constexpr const char* kProResPreviewNote = kProResVideoToolboxPreviewNote;
inline constexpr const char* kProResExportNote = kProResVideoToolboxExportNote;
#else
inline constexpr const char* kProResPreviewNote = kProResFfmpegPreviewNote;
inline constexpr const char* kProResExportNote = kProResFfmpegExportNote;
#endif
} // namespace bloom::media::provider
