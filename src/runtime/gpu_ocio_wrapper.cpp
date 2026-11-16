#include <bloom/runtime/gpu_ocio_wrapper.hpp>

#include <bloom/color/ocio_gpu_program.hpp>

#include <charconv>
#include <cmath>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] bool validIdentifier(const std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    for (const char character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_')) {
            return false;
        }
    }
    return true;
}

// The exact bit-exact straight RGBA8 quantizer proved by the accepted OCIO native tests. It
// reproduces floor(clamp(v, 0, 1) * 255 + 0.5) for the whole binary32 domain, including tiny
// normals and subnormals, without depending on an implicit float->uint conversion.
constexpr std::string_view kQuantizerGlsl = R"(
uint bloom_ocio_quantize(float value)
{
    if (!(value > 0.0)) { return 0u; }
    if (value >= 1.0) { return 255u; }
    if (value < 0.001) { return 0u; }
    int exponent = 0;
    float significand = frexp(value, exponent);
    uint mantissa = uint(significand * 16777216.0);
    uint scaled = mantissa * 255u;
    int shift = 24 - exponent;
    if (shift >= 33) { return 0u; }
    if (shift == 32) { return (scaled >= 0x80000000u) ? 1u : 0u; }
    uint quotient = scaled >> uint(shift);
    uint remainder = scaled & ((1u << uint(shift)) - 1u);
    uint roundBit = 1u << uint(shift - 1);
    return quotient + ((remainder >= roundBit) ? 1u : 0u);
}
)";

[[nodiscard]] GpuOcioWrapperResult failure(const GpuOcioWrapperError error) noexcept {
    GpuOcioWrapperResult result;
    result.error = error;
    return result;
}

// The finished wrapper of a source text. May throw on allocation; the caller is noexcept.
[[nodiscard]] GpuOcioWrapperResult finish(std::string source) {
    GpuOcioWrapperResult result;
    result.source = std::move(source);
    result.entryPoint = std::string(kGpuOcioWrapperEntryPoint);
    result.samplingVersion = std::string(color::kOcioGpuPreciseSamplingVersion);
    result.wrapperVersion = std::string(kGpuOcioWrapperVersion);
    const auto digest = core::Sha256Hasher::hash(
        std::as_bytes(std::span<const char>(result.source.data(), result.source.size())));
    if (!digest.has_value()) {
        return failure(GpuOcioWrapperError::AllocationFailure);
    }
    result.sourceDigest = *digest;
    result.error = GpuOcioWrapperError::None;
    return result;
}

} // namespace

std::string_view gpuOcioWrapperErrorName(const GpuOcioWrapperError error) noexcept {
    switch (error) {
    case GpuOcioWrapperError::None:
        return "none";
    case GpuOcioWrapperError::InvalidProgram:
        return "invalid-program";
    case GpuOcioWrapperError::UnsupportedStage:
        return "unsupported-stage";
    case GpuOcioWrapperError::InvalidFunctionName:
        return "invalid-function-name";
    case GpuOcioWrapperError::UnsupportedDescriptorSet:
        return "unsupported-descriptor-set";
    case GpuOcioWrapperError::AllocationFailure:
        return "allocation-failure";
    case GpuOcioWrapperError::InvalidViewAdjust:
        return "invalid-view-adjust";
    case GpuOcioWrapperError::UnsupportedViewAdjust:
        return "unsupported-view-adjust";
    case GpuOcioWrapperError::InvalidDisplayPacking:
        return "invalid-display-packing";
    }
    return "unknown";
}

// Exact GLSL replica of runtime::ViewAdjust::fromEncoded (the qualified-path post-display
// exposure/gamma from view_adjust.cpp): exposure acts on the decoded linear display light, gamma
// acts on the re-encoded value, and the result is clamped before the byte quantizer. The CPU
// reference is unchanged; the shader evaluates it in binary32 against the OCIO display output.
namespace {
[[nodiscard]] std::string viewAdjustGlsl(const ViewAdjust& adjust) {
    const auto literal = [](const double value) {
        char buffer[40];
        const auto result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
    };
    std::string glsl =
        "\nfloat bloom_ocio_view_adjust(float value)\n{\n"
        "  const float exposure = " +
        literal(adjust.exposure) +
        ";\n"
        "  const float gamma = " +
        literal(adjust.gamma) +
        ";\n"
        "  if (exposure == 0.0) {\n"
        "    float clamped = clamp(value, 0.0, 1.0);\n"
        "    return (gamma == 1.0) ? clamped : pow(clamped, 1.0 / gamma);\n"
        "  }\n"
        "  float linear = (value <= 0.04045) ? value / 12.92 : pow((value + 0.055) / 1.055, "
        "2.4);\n"
        "  float lit = linear * exp2(exposure);\n"
        "  float encoded = (lit <= 0.0031308) ? 12.92 * lit : 1.055 * pow(lit, 1.0 / 2.4) - "
        "0.055;\n"
        "  float clamped = clamp(encoded, 0.0, 1.0);\n"
        "  return (gamma == 1.0) ? clamped : pow(clamped, 1.0 / gamma);\n"
        "}\n";
    return glsl;
}

// #733: the preview packs. The shortest decimal that gives the binary32 value back, so the shader
// constant is the coefficient itself.
[[nodiscard]] std::string floatLiteral(const float value) {
    char buffer[40];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::scientific);
    return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

// The 10-bit code rule and the counter helpers: binary32, in the order written, `precise` (no
// contraction), the same statements as the Bloom Neutral v1 display kernel.
constexpr std::string_view kPreviewPackGlsl = R"(
shared uint bloom_ocio_events[64];
uint bloom_ocio_code10(float value)
{
    if (!(value > 0.0)) { return 0u; }
    if (!(value < 1.0)) { return 1023u; }
    precise float coded = value * 1023.0;
    precise float rounded = coded + 0.5;
    return uint(floor(rounded));
}
uint bloom_ocio_range(float value)
{
    precise float coded = value * 1023.0;
    precise float rounded = coded + 0.5;
    float whole = floor(rounded);
    if (!(whole >= 0.0)) { return 1u; }
    if (!(whole <= 1023.0)) { return 2u; }
    return 0u;
}
bool bloom_ocio_finite(float value)
{
    return (floatBitsToUint(value) & 0x7F800000u) != 0x7F800000u;
}
)";

// One pixel of a preview pack and the workgroup's gathering main(). The pixel function returns
// what it found: bit 0 the pixel refuses 10:10:10:2, bits 1..4 under (red, green, blue, luma),
// bits 5..8 over, bit 9 not finite, bit 16 the error. `wide` is the dispatch's layout word, which
// the host wrote into the status block before the dispatch (kGpuDisplayPackStatusLayout): the
// same program packs either layout.
void emitPreviewPack(std::ostringstream& out, const std::string& functionName,
                     const bool adjusted, const GpuOcioDisplayPacking& packing) {
    out << "uint bloom_ocio_pack_pixel(uint index, bool wide) {\n";
    out << "  ivec2 c = ivec2(int(index % bloom_ocio_push.width), int(index / "
           "bloom_ocio_push.width));\n";
    out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
    out << "  float a = p.a;\n";
    out << "  vec3 s = (a != 0.0) ? vec3(bloom_ocio_cpu_div(p.r, a), "
           "bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a)) : vec3(0.0);\n";
    out << "  if (any(isnan(s)) || any(isinf(s))) { bloom_ocio_output.words[index] = 0u; return "
           "1u << 16u; }\n";
    out << "  vec4 t = " << functionName << "(vec4(s, 1.0));\n";
    out << "  if (any(isnan(t.rgb)) || any(isinf(t.rgb))) { bloom_ocio_output.words[index] = 0u; "
           "return 1u << 16u; }\n";
    // The frame counters: the display color over black, before the Viewer's adjustment and
    // before any clamp.
    out << "  uint events = 0u;\n";
    out << "  precise float signalRed = t.r * a;\n";
    out << "  precise float signalGreen = t.g * a;\n";
    out << "  precise float signalBlue = t.b * a;\n";
    out << "  if (!bloom_ocio_finite(signalRed) || !bloom_ocio_finite(signalGreen) || "
           "!bloom_ocio_finite(signalBlue) || !bloom_ocio_finite(a)) {\n";
    out << "    events |= 1u << 9u;\n";
    out << "  } else {\n";
    out << "    uint rangeRed = bloom_ocio_range(signalRed);\n";
    out << "    uint rangeGreen = bloom_ocio_range(signalGreen);\n";
    out << "    uint rangeBlue = bloom_ocio_range(signalBlue);\n";
    out << "    if (rangeRed == 1u) { events |= 1u << 1u; }\n";
    out << "    if (rangeGreen == 1u) { events |= 1u << 2u; }\n";
    out << "    if (rangeBlue == 1u) { events |= 1u << 3u; }\n";
    out << "    if (rangeRed == 2u) { events |= 1u << 5u; }\n";
    out << "    if (rangeGreen == 2u) { events |= 1u << 6u; }\n";
    out << "    if (rangeBlue == 2u) { events |= 1u << 7u; }\n";
    if (packing.luma.has_value()) {
        out << "    precise float redPart = " << floatLiteral(packing.luma->red)
            << " * signalRed;\n";
        out << "    precise float greenPart = " << floatLiteral(packing.luma->green)
            << " * signalGreen;\n";
        out << "    precise float redGreen = redPart + greenPart;\n";
        out << "    precise float bluePart = " << floatLiteral(packing.luma->blue)
            << " * signalBlue;\n";
        out << "    precise float luma = redGreen + bluePart;\n";
        out << "    uint rangeLuma = bloom_ocio_range(luma);\n";
        out << "    if (rangeLuma == 1u) { events |= 1u << 4u; }\n";
        out << "    if (rangeLuma == 2u) { events |= 1u << 8u; }\n";
    }
    out << "  }\n";
    if (adjusted) {
        out << "  vec3 shown = vec3(bloom_ocio_view_adjust(t.r), bloom_ocio_view_adjust(t.g), "
               "bloom_ocio_view_adjust(t.b));\n";
    } else {
        out << "  vec3 shown = t.rgb;\n";
    }
    // A pixel allows 10:10:10:2 when its alpha is exactly 0 or packs to the 8-bit alpha 255.
    out << "  uint qa = bloom_ocio_quantize(a);\n";
    out << "  bool allowed = a == 0.0 || qa == 255u;\n";
    out << "  if (!allowed) { events |= 1u; }\n";
    // 10:10:10:2: a transparent pixel is the word 0. So is a pixel that is not allowed: the frame
    // is then refused and packed again as 8:8:8:8.
    out << "  if (wide) {\n";
    out << "    uint word = 0u;\n";
    out << "    if (allowed && a != 0.0) {\n";
    out << "      word = bloom_ocio_code10(shown.r) | (bloom_ocio_code10(shown.g) << 10u) | "
           "(bloom_ocio_code10(shown.b) << 20u) | (3u << 30u);\n";
    out << "    }\n";
    out << "    bloom_ocio_output.words[index] = word;\n";
    out << "  } else {\n";
    out << "    uint r = bloom_ocio_quantize(shown.r);\n";
    out << "    uint g = bloom_ocio_quantize(shown.g);\n";
    out << "    uint b = bloom_ocio_quantize(shown.b);\n";
    out << "    bloom_ocio_output.words[index] = r | (g << 8) | (b << 16) | (qa << 24);\n";
    out << "  }\n";
    out << "  return events;\n";
    out << "}\n";
    out << "void main() {\n";
    out << "  const uint bloom_ocio_stride_x = gl_NumWorkGroups.x * gl_WorkGroupSize.x;\n";
    out << "  uint index = gl_GlobalInvocationID.y * bloom_ocio_stride_x + "
           "gl_GlobalInvocationID.x;\n";
    out << "  const bool wide = bloom_ocio_status.flags[" << render::kGpuDisplayPackStatusLayout
        << "] != 0u;\n";
    out << "  uint events = 0u;\n";
    out << "  if (index < bloom_ocio_push.pixelCount) { events = bloom_ocio_pack_pixel(index, "
           "wide); }\n";
    out << "  bloom_ocio_events[gl_LocalInvocationIndex] = events;\n";
    out << "  memoryBarrierShared();\n";
    out << "  barrier();\n";
    out << "  if (gl_LocalInvocationIndex != 0u) { return; }\n";
    out << "  uint errors = 0u;\n";
    out << "  uint misfit = 0u;\n";
    out << "  uint counts[9] = uint[9](0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);\n";
    out << "  for (uint member = 0u; member < " << kGpuOcioWrapperWorkgroupSize
        << "u; ++member) {\n";
    out << "    uint found = bloom_ocio_events[member];\n";
    out << "    errors |= found >> 16u;\n";
    out << "    misfit |= found & 1u;\n";
    out << "    for (uint slot = 0u; slot < 9u; ++slot) { counts[slot] += (found >> (slot + 1u)) "
           "& 1u; }\n";
    out << "  }\n";
    out << "  if (errors != 0u) { atomicOr(bloom_ocio_status.flags[0], 1u); }\n";
    out << "  if (misfit != 0u) { atomicOr(bloom_ocio_status.flags[1], 1u); }\n";
    out << "  for (uint slot = 0u; slot < 9u; ++slot) {\n";
    out << "    if (counts[slot] != 0u) { atomicAdd(bloom_ocio_status.flags[2u + slot], "
           "counts[slot]); }\n";
    out << "  }\n";
    out << "}\n";
}

} // namespace

GpuOcioWrapperResult buildGpuOcioWrapperGlsl(const render::OcioGpuProgramDesc& program,
                                             const ViewAdjust viewAdjust,
                                             const GpuOcioDisplayPacking& packing) noexcept {
    try {
        if (program.shaderText.empty()) {
            return failure(GpuOcioWrapperError::InvalidProgram);
        }
        if (!validIdentifier(program.functionName)) {
            return failure(GpuOcioWrapperError::InvalidFunctionName);
        }
        if (program.descriptorSetIndex != 0) {
            return failure(GpuOcioWrapperError::UnsupportedDescriptorSet);
        }
        if (!viewAdjust.valid()) {
            return failure(GpuOcioWrapperError::InvalidViewAdjust);
        }
        const bool display = program.stage == render::OcioGpuProgramStage::DisplayPacking;
        const bool effect = program.stage == render::OcioGpuProgramStage::ProcessEffect;
        const bool neighborhood = program.stage == render::OcioGpuProgramStage::ProcessNeighborhood;
        if (!display && !effect && !neighborhood) {
            return failure(GpuOcioWrapperError::UnsupportedStage);
        }
        // #724: the scopes' Display signal is the display transform as a float effect with the
        // Viewer's adjustment applied unquantized; the scopes' XYZ step transforms the stored
        // color as it is. Every other effect keeps its text.
        const bool scopeDisplay =
            effect && program.semanticsId == color::kOcioGpuScopeDisplaySemanticsId;
        const bool scopeXyz = effect && program.semanticsId == color::kOcioGpuScopeXyzSemanticsId;
        if ((effect || neighborhood) && !scopeDisplay && !viewAdjust.neutral()) {
            return failure(GpuOcioWrapperError::UnsupportedViewAdjust);
        }
        const bool adjusted = (display || scopeDisplay) && !viewAdjust.neutral();
        // #733: only a display pack can be a preview pack, and its coefficients are finite.
        if (!packing.valid() || (packing.preview() && !display)) {
            return failure(GpuOcioWrapperError::InvalidDisplayPacking);
        }
        if (packing.luma.has_value() &&
            !(std::isfinite(packing.luma->red) && std::isfinite(packing.luma->green) &&
              std::isfinite(packing.luma->blue))) {
            return failure(GpuOcioWrapperError::InvalidDisplayPacking);
        }
        const bool preview = display && packing.preview();

        // Shared versioned per-sampler precise-sampling adapter. OCIO's generated body samples LUT
        // resources with the built-in texture(), whose hardware filtering is not full precision;
        // the adapter emits one texelFetch-based function per reflected sampler and rewrites the
        // body's texture() calls to it while leaving the generated transform body byte-for-byte
        // intact.
        const auto sampling = color::ocioGpuSamplingGlslFor(program);

        std::ostringstream out;
        out << "#version 460\n";
        out << "layout(local_size_x = " << kGpuOcioWrapperWorkgroupSize << ") in;\n";
        out << "layout(set = 1, binding = 0, rgba32f) uniform readonly image2D bloom_ocio_input;\n";
        if (display) {
            out << "layout(set = 1, binding = 1, std430) buffer BloomOcioOutput { uint words[]; } "
                   "bloom_ocio_output;\n";
        } else {
            out << "layout(set = 1, binding = 1, rgba32f) uniform writeonly image2D "
                   "bloom_ocio_output;\n";
        }
        out << "layout(set = 1, binding = 2, std430) buffer BloomOcioStatus { uint flags[]; } "
               "bloom_ocio_status;\n";
        out << "layout(push_constant) uniform BloomOcioPush { uint pixelCount; uint width; uint "
               "height; "
               "} bloom_ocio_push;\n";
        out << sampling.preamble;
        // A per-op specialized body (when present) replaces the generated body; the extracted
        // descriptor and its digest/content identity are never modified.
        const std::string& body =
            sampling.shaderBody.empty() ? program.shaderText : sampling.shaderBody;
        out << body;
        if (body.back() != '\n') {
            out << '\n';
        }
        out << sampling.definitions;
        out << color::ocioGpuPreciseDivisionGlsl();
        if (display) {
            out << kQuantizerGlsl;
        }
        if (adjusted) {
            out << viewAdjustGlsl(viewAdjust);
        }
        if (preview) {
            // The preview packs have their own pixel function and gathering main().
            out << kPreviewPackGlsl;
            emitPreviewPack(out, program.functionName, adjusted, packing);
            return finish(out.str());
        }
        out << "void main() {\n";
        // Capacity-safe 2D flattening: strideX is the exact X invocation extent of the dispatch
        // (groupsX * 64), so a 1D dispatch (groupsY == 1) reduces to the previous
        // gl_GlobalInvocationID.x mapping, while a bounded 2D grid never exceeds the device's
        // maxComputeWorkGroupCount[0]. The pixelCount guard discards the tail row.
        out << "  const uint bloom_ocio_stride_x = gl_NumWorkGroups.x * gl_WorkGroupSize.x;\n";
        out << "  uint index = gl_GlobalInvocationID.y * bloom_ocio_stride_x + "
               "gl_GlobalInvocationID.x;\n";
        out << "  if (index >= bloom_ocio_push.pixelCount) { return; }\n";
        out << "  ivec2 c = ivec2(int(index % bloom_ocio_push.width), int(index / "
               "bloom_ocio_push.width));\n";
        if (neighborhood) {
            out << "  vec4 p = " << program.functionName << "(c);\n";
            out << "  if (any(isnan(p)) || any(isinf(p)) || p.a < 0.0 || p.a > 1.0) { "
                   "bloom_ocio_status.flags[0] = 1u; return; }\n";
            // Match Rgba32f::fromPremultiplied after each CPU neighborhood pass, including
            // coverage that underflows to zero. Validate first so invalid HDR still fails.
            out << "  if (p.a == 0.0) { p = vec4(0.0); }\n";
            out << "  imageStore(bloom_ocio_output, c, p);\n";
        } else if (effect && program.semanticsId == color::kOcioGpuPointwiseAffineSemanticsId) {
            // The affine matrix carries its offset in the alpha column. Apply it directly to
            // associated RGB so legal HDR pixels never overflow an unpremultiply intermediate.
            out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
            out << "  if (p.a == 0.0) { imageStore(bloom_ocio_output, c, p); return; }\n";
            out << "  vec4 t = " << program.functionName << "(p);\n";
            out << "  if (any(isnan(t.rgb)) || any(isinf(t.rgb))) { "
                   "bloom_ocio_status.flags[0] = 1u; return; }\n";
            out << "  imageStore(bloom_ocio_output, c, vec4(t.rgb, p.a));\n";
        } else if (scopeXyz) {
            // The signal over black is transformed as stored: no division by alpha. A pixel
            // whose XYZ is not finite has no chromaticity, which the measure counts from a zero.
            out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
            out << "  vec4 t = " << program.functionName << "(vec4(p.rgb, 1.0));\n";
            out << "  if (any(isnan(t.rgb)) || any(isinf(t.rgb))) { t = vec4(0.0); }\n";
            out << "  imageStore(bloom_ocio_output, c, vec4(t.rgb, p.a));\n";
        } else {
            out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
            out << "  float a = p.a;\n";
            if (effect) {
                // Exact CPU image-effect semantics: an alpha-zero source pixel is copied through
                // unchanged, including any hidden RGB, and is never un-premultiplied or
                // transformed.
                out << "  if (a == 0.0) { imageStore(bloom_ocio_output, c, p); return; }\n";
                out << "  vec3 s = vec3(bloom_ocio_cpu_div(p.r, a), bloom_ocio_cpu_div(p.g, a), "
                       "bloom_ocio_cpu_div(p.b, a));\n";
            } else {
                out << "  vec3 s = (a != 0.0) ? vec3(bloom_ocio_cpu_div(p.r, a), "
                       "bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a)) : vec3(0.0);\n";
            }
            out << "  if (any(isnan(s)) || any(isinf(s))) { bloom_ocio_status.flags[0] = 1u; "
                   "return; "
                   "}\n";
            out << "  vec4 t = " << program.functionName << "(vec4(s, 1.0));\n";
            out << "  if (any(isnan(t.rgb)) || any(isinf(t.rgb))) { bloom_ocio_status.flags[0] = "
                   "1u; "
                   "return; }\n";
            if (display) {
                if (adjusted) {
                    out << "  vec3 adjusted = vec3(bloom_ocio_view_adjust(t.r), "
                           "bloom_ocio_view_adjust(t.g), bloom_ocio_view_adjust(t.b));\n";
                    out << "  uint r = bloom_ocio_quantize(adjusted.r);\n";
                    out << "  uint g = bloom_ocio_quantize(adjusted.g);\n";
                    out << "  uint b = bloom_ocio_quantize(adjusted.b);\n";
                } else {
                    out << "  uint r = bloom_ocio_quantize(t.r);\n";
                    out << "  uint g = bloom_ocio_quantize(t.g);\n";
                    out << "  uint b = bloom_ocio_quantize(t.b);\n";
                }
                out << "  uint qa = bloom_ocio_quantize(a);\n";
                out << "  bloom_ocio_output.words[index] = r | (g << 8) | (b << 16) | (qa << "
                       "24);\n";
            } else {
                // The CPU reference validates the stored premultiplied pixel, not just the
                // straight-RGB intermediate. Never publish a non-finite resident process pixel.
                if (adjusted) {
                    out << "  t.rgb = vec3(bloom_ocio_view_adjust(t.r), "
                           "bloom_ocio_view_adjust(t.g), bloom_ocio_view_adjust(t.b));\n";
                }
                out << "  vec3 premultiplied = bloom_ocio_cpu_premul(t.rgb, a);\n";
                out << "  if (any(isnan(premultiplied)) || any(isinf(premultiplied))) { "
                       "bloom_ocio_status.flags[0] = 1u; return; }\n";
                out << "  imageStore(bloom_ocio_output, c, vec4(premultiplied, a));\n";
            }
        }
        out << "}\n";
        return finish(out.str());
    } catch (...) {
        // This function is noexcept by contract; an allocation failure (or any other exception from
        // the sampling adapter / string building) is reported as a typed error, never a terminate.
        return failure(GpuOcioWrapperError::AllocationFailure);
    }
}

} // namespace bloom::runtime
