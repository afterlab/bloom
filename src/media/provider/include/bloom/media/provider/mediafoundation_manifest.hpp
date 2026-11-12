#pragma once

#include <bloom/media/provider/protocol.hpp>

namespace bloom::media::provider {

// The handshake of the media worker's Media Foundation generation on Windows (ADR 0035; issue
// #370). The generation keeps the worker's libavformat for the container and codes H.264 and HEVC
// pictures with the system's transforms, so the handshake has ffmpegHandshake's shape with the
// Windows framework's identities: trust domain windows-media-foundation, picture decode for H.264
// and HEVC only, and stored audio only.
//
// The decoders and encoders of Windows differ from one computer to the next (the HEVC decoder is a
// separate component, and the encoders belong to the graphics driver or to Windows), so the
// handshake declares what the generation implements; what a computer lacks is refused by the
// worker with a reason, and the provider chain hands that request to the software generation.
[[nodiscard]] Handshake mediaFoundationHandshake();

// The exact pipeline digest for one declared Media Foundation capability.
[[nodiscard]] PipelineQualificationV1
mediaFoundationPipeline(const ProviderDeclaration& declaration);

} // namespace bloom::media::provider
