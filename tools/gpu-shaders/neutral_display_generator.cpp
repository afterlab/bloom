// Bloom Neutral v1 display compute-shader generator (offline, standalone).
//
// Reads exactly one explicitly-passed frozen OCIO asset, verifies its raw SHA-256 against the
// configure-time pin, rebuilds the BloomOcioRevision v1 envelope digest independently, parses
// the config from those exact bytes with a freshly created empty OCIO Context (never the
// environment), builds the same default DisplayViewTransform the CPU display path builds,
// extracts the GLSL_VK_4_6 GPU shader, rejects any texture/uniform/dynamic-property resource,
// and emits a deterministic compute shader plus a manifest.
//
// The output is an offline artifact. This tool does not execute the shader, and the artifacts
// contain no machine paths or timestamps.

#include <OpenColorIO/OpenColorIO.h>

#include <bloom/core/sha256.hpp>

#include "bloom_neutral_v1_pins.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

namespace {

[[nodiscard]] std::span<const std::byte> bytesOf(const std::string& text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] std::string digestToHex(const bloom::core::Sha256Digest& digest) {
    const auto hex = digest.toLowercaseHex();
    return std::string(hex.data(), hex.size());
}

[[nodiscard]] std::string sha256Hex(const std::string& bytes) {
    const auto digest = bloom::core::Sha256Hasher::hash(bytesOf(bytes));
    if (!digest.has_value()) {
        throw std::runtime_error("SHA-256 input exceeds the encodable message length");
    }
    return digestToHex(*digest);
}

// BloomOcioRevision version 1 envelope from docs/architecture/color-management.md.
[[nodiscard]] std::string bloomOcioRevisionV1Hex(const std::string& payload) {
    bloom::core::Sha256Hasher hasher;
    const auto feed = [&hasher](const std::span<const std::byte> bytes) {
        if (!hasher.update(bytes)) {
            throw std::runtime_error(
                "BloomOcioRevision input exceeds the encodable message length");
        }
    };

    static constexpr char kDomain[] = "BloomOcioRevision";
    feed(std::as_bytes(std::span(kDomain)));
    const std::uint8_t revisionVersion[2]{0x00U, 0x01U}; // u16(1) big-endian
    feed(std::as_bytes(std::span(revisionVersion)));
    const std::uint8_t locator[1]{static_cast<std::uint8_t>(BLOOM_NEUTRAL_V1_LOCATOR_KIND)};
    feed(std::as_bytes(std::span(locator)));
    const std::uint64_t byteCount = payload.size();
    std::array<std::uint8_t, 8> countBytes{};
    for (std::size_t i = 0; i < countBytes.size(); ++i) {
        countBytes[i] = static_cast<std::uint8_t>((byteCount >> (56U - (8U * i))) & 0xffU);
    }
    feed(std::as_bytes(std::span(countBytes)));
    feed(bytesOf(payload));
    return digestToHex(hasher.finalize());
}

[[nodiscard]] std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open asset for reading: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open output for writing: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("failed writing output: " + path.string());
    }
}

constexpr const char* kProcessColorSpaceName = "lin_rec709_scene";
constexpr const char* kProcessInteropId = "lin_rec709_scene";
constexpr const char* kDisplayName = "srgb_rec709_display";
constexpr const char* kViewName = "srgb_rec709_display";
constexpr const char* kOutputColorSpaceName = "srgb_rec709_display";
constexpr const char* kOutputInteropId = "srgb_rec709_display";
constexpr const char* kOcioFunctionName = "bloom_neutral_v1_display";
constexpr const char* kResourcePrefix = "bloom_neutral_v1_";
constexpr const char* kEntrypoint = "main";

constexpr std::uint32_t kErrorInputNonFinite = 1U << 0U;
constexpr std::uint32_t kErrorUnpremultiplyNonFinite = 1U << 1U;
constexpr std::uint32_t kErrorOutputNonFinite = 1U << 2U;
constexpr std::uint32_t kErrorInputSubnormal = 1U << 3U;

[[nodiscard]] bool interopIdMatches(const OCIO::ConstColorSpaceRcPtr& colorSpace,
                                    const char* expected) {
    if (!colorSpace) {
        return false;
    }
    const char* const interopId = colorSpace->getInteropID();
    return interopId != nullptr && std::string(interopId) == expected;
}

struct GpuShaderProducts {
    std::string shaderText;
    std::string processorCacheId;
    std::string gpuProcessorCacheId;
};

[[nodiscard]] GpuShaderProducts extractGpuShader(const OCIO::ConstConfigRcPtr& config) {
    if (config->getNumEnvironmentVars() != 0) {
        throw std::runtime_error(
            "Bloom Neutral v1 config declares context variables; refusing ambient influence");
    }

    const auto processColorSpace = config->getColorSpace(kProcessColorSpaceName);
    if (!interopIdMatches(processColorSpace, kProcessInteropId)) {
        throw std::runtime_error("process color space is not the expected Color Interop ID");
    }
    const auto outputColorSpace = config->getColorSpace(kOutputColorSpaceName);
    if (!interopIdMatches(outputColorSpace, kOutputInteropId)) {
        throw std::runtime_error("output color space is not the expected Color Interop ID");
    }
    const char* const displayViewColorSpace =
        config->getDisplayViewColorSpaceName(kDisplayName, kViewName);
    if (displayViewColorSpace == nullptr ||
        std::string(displayViewColorSpace) != kOutputColorSpaceName) {
        throw std::runtime_error("display/view does not resolve to the expected output space");
    }

    // Exact same transform and direction as ocio_cpu_display_processor.cpp, built with a freshly
    // created, never-environment-loaded empty Context.
    const OCIO::ConstContextRcPtr emptyContext = OCIO::Context::Create();
    auto transform = OCIO::DisplayViewTransform::Create();
    transform->setSrc(kProcessColorSpaceName);
    transform->setDisplay(kDisplayName);
    transform->setView(kViewName);
    OCIO::ConstProcessorRcPtr processor =
        config->getProcessor(emptyContext, transform, OCIO::TRANSFORM_DIR_FORWARD);
    if (!processor) {
        throw std::runtime_error("OCIO returned a null processor");
    }

    OCIO::ConstGPUProcessorRcPtr gpuProcessor = processor->getDefaultGPUProcessor();
    if (!gpuProcessor) {
        throw std::runtime_error("OCIO returned a null GPU processor");
    }

    OCIO::GpuShaderDescRcPtr description = OCIO::GpuShaderDesc::CreateShaderDesc();
    description->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
    description->setFunctionName(kOcioFunctionName);
    description->setResourcePrefix(kResourcePrefix);
    gpuProcessor->extractGpuShaderInfo(description);

    if (description->getNumTextures() != 0U || description->getNum3DTextures() != 0U ||
        description->getNumUniforms() != 0U || description->getNumDynamicProperties() != 0U ||
        description->getUniformBufferSize() != 0U) {
        throw std::runtime_error(
            "OCIO shader carries textures, uniforms, or dynamic properties; this bounded first "
            "shader rejects them");
    }

    const char* const shaderText = description->getShaderText();
    if (shaderText == nullptr || *shaderText == '\0') {
        throw std::runtime_error("OCIO returned empty shader text");
    }

    GpuShaderProducts products;
    products.shaderText = shaderText;
    products.processorCacheId = processor->getCacheID();
    products.gpuProcessorCacheId = gpuProcessor->getCacheID();
    return products;
}

[[nodiscard]] std::string buildShaderSource(const GpuShaderProducts& products,
                                            const std::string& rawDigest,
                                            const std::string& revisionDigest,
                                            const std::string& ocioVersion) {
    const std::string header =
        "#version 460\n"
        "// Bloom Neutral v1 display compute shader. Offline-generated; do not edit.\n"
        "// operation: BloomNeutralV1Display\n"
        "// source-config-raw-sha256: " +
        rawDigest +
        "\n"
        "// source-config-revision-sha256: " +
        revisionDigest +
        "\n"
        "// opencolorio-version: " +
        ocioVersion +
        "\n"
        "// processor-cache-id: " +
        products.processorCacheId +
        "\n"
        "// ocio-function: " +
        std::string(kOcioFunctionName) +
        "\n"
        "layout(local_size_x = 256) in;\n"
        "\n"
        "layout(std430, set = 0, binding = 0) readonly buffer BloomNeutralV1Input {\n"
        "    vec4 pixels[];\n"
        "} bloom_input;\n"
        "\n"
        "layout(std430, set = 0, binding = 1) writeonly buffer BloomNeutralV1Output {\n"
        "    uint words[];\n"
        "} bloom_output;\n"
        "\n"
        "// The status block, zeroed by the host: word 0 error bits (nonzero rejects the frame),\n"
        "// word 1 nonzero when a pixel does not allow the 10:10:10:2 layout, words 2..5 pixels\n"
        "// under 0 (red, green, blue, luma), words 6..9 pixels over 1, word 10 pixels whose\n"
        "// signal is not finite, word 11 reserved.\n"
        "layout(std430, set = 0, binding = 2) buffer BloomNeutralV1Status {\n"
        "    uint words[];\n"
        "} bloom_status;\n"
        "\n"
        "layout(push_constant) uniform BloomNeutralV1PushConstants {\n"
        "    uint pixelCount;\n"
        "    uint pixelLayout; // 0: straight 8:8:8:8, 1: 10:10:10:2\n"
        "    float lumaRed;\n"
        "    float lumaGreen;\n"
        "    float lumaBlue;\n"
        "    uint lumaCounted;\n"
        "} bloom_push;\n"
        "\n"
        "// What each invocation of a workgroup found, summed by the group's first invocation so\n"
        "// the status block takes one atomic per group and word, not one per pixel.\n"
        "shared uint bloom_events[256];\n"
        "\n"
        "// Exact replica of the CPU straight-RGBA8 rule\n"
        "//   floor(clamp(value, 0, 1) * 255.0 + 0.5)\n"
        "// evaluated with the exact binary64 value of the binary32 input. The input significand\n"
        "// has at most 24 bits and 255 is odd, so value * 255 is exactly representable in\n"
        "// binary64; frexp plus integer arithmetic reproduces the binary64 floor, half-up, "
        "without\n"
        "// requiring shaderFloat64. The only exact tie is value == 0.5 and it rounds up to 128.\n"
        "uint bloom_neutral_v1_quantize_byte(float value)\n"
        "{\n"
        "    if (!(value > 0.0)) { return 0u; }\n"
        "    if (value >= 1.0) { return 255u; }\n"
        "    if (value < 0.001) { return 0u; }\n"
        "    int exponent = 0;\n"
        "    float significand = frexp(value, exponent);\n"
        "    uint mantissa = uint(significand * 16777216.0);\n"
        "    uint scaled = mantissa * 255u;\n"
        "    int shift = 24 - exponent;\n"
        "    if (shift >= 33) { return 0u; }\n"
        "    if (shift == 32) { return (scaled >= 0x80000000u) ? 1u : 0u; }\n"
        "    uint quotient = scaled >> uint(shift);\n"
        "    uint remainder = scaled & ((1u << uint(shift)) - 1u);\n"
        "    uint roundBit = 1u << uint(shift - 1);\n"
        "    return quotient + ((remainder >= roundBit) ? 1u : 0u);\n"
        "}\n"
        "\n"
        "// The 10-bit pack rule, the scope code rule: floor(clamp(value, 0, 1) * 1023 + 0.5) in\n"
        "// binary32, in the order written, with no contraction. A value that is not a number\n"
        "// packs to 0.\n"
        "uint bloom_neutral_v1_code10(float value)\n"
        "{\n"
        "    if (!(value > 0.0)) { return 0u; }\n"
        "    if (!(value < 1.0)) { return 1023u; }\n"
        "    precise float coded = value * 1023.0;\n"
        "    precise float rounded = coded + 0.5;\n"
        "    return uint(floor(rounded));\n"
        "}\n"
        "\n"
        "// Where a signal value falls by that rule: 0 inside the axis, 1 under, 2 over.\n"
        "uint bloom_neutral_v1_range(float value)\n"
        "{\n"
        "    precise float coded = value * 1023.0;\n"
        "    precise float rounded = coded + 0.5;\n"
        "    float whole = floor(rounded);\n"
        "    if (!(whole >= 0.0)) { return 1u; }\n"
        "    if (!(whole <= 1023.0)) { return 2u; }\n"
        "    return 0u;\n"
        "}\n"
        "\n"
        "bool bloom_neutral_v1_finite(float value)\n"
        "{\n"
        "    return (floatBitsToUint(value) & 0x7F800000u) != 0x7F800000u;\n"
        "}\n"
        "\n";

    const std::string errorReturn =
        "    if (errors != 0u) {\n"
        "        bloom_output.words[index] = 0u;\n"
        "        return errors << 16u;\n"
        "    }\n";
    const std::string body =
        "\n// Packs one pixel and returns what it found: bit 0 the pixel does not allow the\n"
        "// 10:10:10:2 layout, bits 1..4 under (red, green, blue, luma), bits 5..8 over, bit 9\n"
        "// not finite, bits 16..19 the error bits.\n"
        "uint bloom_neutral_v1_pixel(uint index)\n"
        "{\n"
        "    vec4 premultiplied = bloom_input.pixels[index];\n"
        "    uint errors = 0u;\n"
        "    if (any(isnan(premultiplied)) || any(isinf(premultiplied))) {\n"
        "        errors |= " +
        std::to_string(kErrorInputNonFinite) +
        "u;\n"
        "    }\n"
        "    // Any nonzero subnormal input lane rejects the whole frame until denormal\n"
        "    // preservation is qualified; the intended route for such a frame is a CPU fallback.\n"
        "    uvec4 inputBits = floatBitsToUint(premultiplied);\n"
        "    uvec4 exponentBits = inputBits & uvec4(0x7F800000u);\n"
        "    uvec4 mantissaBits = inputBits & uvec4(0x007FFFFFu);\n"
        "    bvec4 inputIsSubnormal = bvec4(exponentBits.x == 0u && mantissaBits.x != 0u,\n"
        "                                   exponentBits.y == 0u && mantissaBits.y != 0u,\n"
        "                                   exponentBits.z == 0u && mantissaBits.z != 0u,\n"
        "                                   exponentBits.w == 0u && mantissaBits.w != 0u);\n"
        "    if (any(inputIsSubnormal)) {\n"
        "        errors |= " +
        std::to_string(kErrorInputSubnormal) +
        "u;\n"
        "    }\n" +
        errorReturn +
        "\n"
        "    float alpha = premultiplied.a;\n"
        "    vec3 straight = vec3(0.0);\n"
        "    if (alpha != 0.0) {\n"
        "        straight = premultiplied.rgb / alpha;\n"
        "        if (any(isnan(straight)) || any(isinf(straight))) {\n"
        "            errors |= " +
        std::to_string(kErrorUnpremultiplyNonFinite) +
        "u;\n"
        "        }\n"
        "    }\n" +
        errorReturn +
        "\n"
        "    // OCIO affects RGB only. The alpha lane fed to the transform is 1.0 and the "
        "transform's\n"
        "    // alpha result is discarded because Bloom carries the original alpha unchanged.\n"
        "    vec3 displayRgb = " +
        std::string(kOcioFunctionName) +
        "(vec4(straight, 1.0)).rgb;\n"
        "    if (any(isnan(displayRgb)) || any(isinf(displayRgb))) {\n"
        "        errors |= " +
        std::to_string(kErrorOutputNonFinite) +
        "u;\n"
        "    }\n" +
        errorReturn +
        "\n"
        "    // The frame counters: the display color over black, before any clamp.\n"
        "    uint events = 0u;\n"
        "    precise float signalRed = displayRgb.r * alpha;\n"
        "    precise float signalGreen = displayRgb.g * alpha;\n"
        "    precise float signalBlue = displayRgb.b * alpha;\n"
        "    if (!bloom_neutral_v1_finite(signalRed) || !bloom_neutral_v1_finite(signalGreen) ||\n"
        "        !bloom_neutral_v1_finite(signalBlue) || !bloom_neutral_v1_finite(alpha)) {\n"
        "        events |= 1u << 9u;\n"
        "    } else {\n"
        "        uint rangeRed = bloom_neutral_v1_range(signalRed);\n"
        "        uint rangeGreen = bloom_neutral_v1_range(signalGreen);\n"
        "        uint rangeBlue = bloom_neutral_v1_range(signalBlue);\n"
        "        if (rangeRed == 1u) { events |= 1u << 1u; }\n"
        "        if (rangeGreen == 1u) { events |= 1u << 2u; }\n"
        "        if (rangeBlue == 1u) { events |= 1u << 3u; }\n"
        "        if (rangeRed == 2u) { events |= 1u << 5u; }\n"
        "        if (rangeGreen == 2u) { events |= 1u << 6u; }\n"
        "        if (rangeBlue == 2u) { events |= 1u << 7u; }\n"
        "        if (bloom_push.lumaCounted != 0u) {\n"
        "            precise float redPart = bloom_push.lumaRed * signalRed;\n"
        "            precise float greenPart = bloom_push.lumaGreen * signalGreen;\n"
        "            precise float redGreen = redPart + greenPart;\n"
        "            precise float bluePart = bloom_push.lumaBlue * signalBlue;\n"
        "            precise float luma = redGreen + bluePart;\n"
        "            uint rangeLuma = bloom_neutral_v1_range(luma);\n"
        "            if (rangeLuma == 1u) { events |= 1u << 4u; }\n"
        "            if (rangeLuma == 2u) { events |= 1u << 8u; }\n"
        "        }\n"
        "    }\n"
        "\n"
        "    // A pixel allows the 10:10:10:2 layout when its alpha is exactly 0 or packs to the\n"
        "    // 8-bit alpha 255.\n"
        "    uint alphaByte = bloom_neutral_v1_quantize_byte(alpha);\n"
        "    bool allowed = alpha == 0.0 || alphaByte == 255u;\n"
        "    if (!allowed) { events |= 1u; }\n"
        "    if (bloom_push.pixelLayout == 1u) {\n"
        "        // A transparent pixel is the word 0. So is a pixel that is not allowed: the\n"
        "        // frame is then refused and packed again as 8:8:8:8.\n"
        "        uint word = 0u;\n"
        "        if (allowed && alpha != 0.0) {\n"
        "            word = bloom_neutral_v1_code10(displayRgb.r) |\n"
        "                   (bloom_neutral_v1_code10(displayRgb.g) << 10u) |\n"
        "                   (bloom_neutral_v1_code10(displayRgb.b) << 20u) | (3u << 30u);\n"
        "        }\n"
        "        bloom_output.words[index] = word;\n"
        "        return events;\n"
        "    }\n"
        "    uint red = bloom_neutral_v1_quantize_byte(displayRgb.r);\n"
        "    uint green = bloom_neutral_v1_quantize_byte(displayRgb.g);\n"
        "    uint blue = bloom_neutral_v1_quantize_byte(displayRgb.b);\n"
        "    bloom_output.words[index] = red | (green << 8) | (blue << 16) | (alphaByte << 24);\n"
        "    return events;\n"
        "}\n"
        "\n"
        "void main()\n"
        "{\n"
        "    uint index = gl_GlobalInvocationID.x;\n"
        "    uint events = 0u;\n"
        "    if (index < bloom_push.pixelCount) {\n"
        "        events = bloom_neutral_v1_pixel(index);\n"
        "    }\n"
        "    bloom_events[gl_LocalInvocationIndex] = events;\n"
        "    memoryBarrierShared();\n"
        "    barrier();\n"
        "    if (gl_LocalInvocationIndex != 0u) { return; }\n"
        "\n"
        "    uint errors = 0u;\n"
        "    uint misfit = 0u;\n"
        "    uint counts[9] = uint[9](0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);\n"
        "    for (uint member = 0u; member < 256u; ++member) {\n"
        "        uint found = bloom_events[member];\n"
        "        errors |= found >> 16u;\n"
        "        misfit |= found & 1u;\n"
        "        for (uint slot = 0u; slot < 9u; ++slot) {\n"
        "            counts[slot] += (found >> (slot + 1u)) & 1u;\n"
        "        }\n"
        "    }\n"
        "    if (errors != 0u) { atomicOr(bloom_status.words[0], errors); }\n"
        "    if (misfit != 0u) { atomicOr(bloom_status.words[1], 1u); }\n"
        "    for (uint slot = 0u; slot < 9u; ++slot) {\n"
        "        if (counts[slot] != 0u) { atomicAdd(bloom_status.words[2u + slot], counts[slot]); }\n"
        "    }\n"
        "}\n";

    return header + products.shaderText + "\n" + body;
}

[[nodiscard]] std::string buildManifest(const GpuShaderProducts& products,
                                        const std::string& rawDigest,
                                        const std::string& revisionDigest,
                                        const std::string& ocioVersion,
                                        const std::string& shaderDigest) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"bloom.gpu.display-shader.manifest.v1\",\n";
    out << "  \"generator\": \"tools/gpu-shaders/neutral_display_generator\",\n";
    out << "  \"generatorRevision\": 2,\n";
    out << "  \"operation\": \"BloomNeutralV1Display\",\n";
    out << "  \"entrypoint\": \"" << kEntrypoint << "\",\n";
    out << "  \"ocioFunction\": \"" << kOcioFunctionName << "\",\n";
    out << "  \"ocioVersion\": \"" << ocioVersion << "\",\n";
    out << "  \"sourceConfig\": {\n";
    out << "    \"rawSha256\": \"" << rawDigest << "\",\n";
    out << "    \"revisionSha256\": \"" << revisionDigest << "\",\n";
    out << "    \"revisionAlgorithm\": \"sha256\",\n";
    out << "    \"revisionVersion\": " << BLOOM_NEUTRAL_V1_REVISION_VERSION << ",\n";
    out << "    \"locatorKind\": " << BLOOM_NEUTRAL_V1_LOCATOR_KIND << ",\n";
    out << "    \"payloadByteCount\": " << BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT << "\n";
    out << "  },\n";
    out << "  \"processorCacheId\": \"" << products.processorCacheId << "\",\n";
    out << "  \"gpuProcessorCacheId\": \"" << products.gpuProcessorCacheId << "\",\n";
    out << "  \"shader\": {\n";
    out << "    \"language\": \"GLSL_460\",\n";
    out << "    \"stage\": \"compute\",\n";
    out << "    \"sha256\": \"" << shaderDigest << "\",\n";
    out << "    \"localSizeX\": 256,\n";
    out << "    \"textures\": 0,\n";
    out << "    \"textures3D\": 0,\n";
    out << "    \"uniforms\": 0,\n";
    out << "    \"dynamicProperties\": 0\n";
    out << "  },\n";
    out << "  \"bindings\": [\n";
    out << "    {\"set\": 0, \"binding\": 0, \"kind\": \"storage-buffer\", \"access\": "
           "\"readonly\", "
           "\"element\": \"vec4\", \"role\": \"premultiplied-rgba32f-input\"},\n";
    out << "    {\"set\": 0, \"binding\": 1, \"kind\": \"storage-buffer\", \"access\": "
           "\"writeonly\", "
           "\"element\": \"uint\", \"packing\": {\"straight-rgba8\": "
           "\"R|(G<<8)|(B<<16)|(A<<24)\", \"preview-rgb10-a2\": "
           "\"R|(G<<10)|(B<<20)|(3<<30), or 0 for a transparent pixel\"}, "
           "\"role\": \"packed-display-output\"},\n";
    out << "    {\"set\": 0, \"binding\": 2, \"kind\": \"storage-buffer\", \"access\": "
           "\"readwrite\", "
           "\"element\": \"uint\", \"role\": \"status-block\", \"words\": 12, "
           "\"layout\": {\"errors\": 0, \"refusesRgb10A2\": 1, \"under\": [2, 3, 4, 5], "
           "\"over\": [6, 7, 8, 9], \"notFinite\": 10, \"reserved\": 11}, "
           "\"bits\": {\"inputNonFinite\": "
        << kErrorInputNonFinite << ", \"inputSubnormal\": " << kErrorInputSubnormal
        << ", \"unpremultiplyNonFinite\": " << kErrorUnpremultiplyNonFinite
        << ", \"outputNonFinite\": " << kErrorOutputNonFinite << "}}\n";
    out << "  ],\n";
    out << "  \"pushConstant\": {\"byteCount\": 24, \"fields\": [\"uint32 pixelCount\", "
           "\"uint32 pixelLayout\", \"float lumaRed\", \"float lumaGreen\", \"float "
           "lumaBlue\", \"uint32 lumaCounted\"]},\n";
    out << "  \"contracts\": {\n";
    out << "    \"rgbToleranceCodes\": 1,\n";
    out << "    \"rgbToleranceNote\": \"OCIO GPU vs CPU float rounding may differ by at most one "
           "code per channel in either layout (one 8-bit code, one 10-bit code); alpha is "
           "exact\",\n";
    out << "    \"alphaToleranceCodes\": 0,\n";
    out << "    \"alphaRule\": "
           "\"floor(clamp(alpha,0,1)*255+0.5) with the exact binary64 value of the binary32 "
           "alpha; reproduced without shaderFloat64\",\n";
    out << "    \"zeroAlphaRgb\": \"+0\",\n";
    out << "    \"alphaPreserved\": true,\n";
    out << "    \"ocioChannels\": \"rgb\",\n";
    out << "    \"nonFinitePolicy\": \"atomicOr an error bit and write a zero placeholder; the "
           "consumer must not publish a frame while status word 0 is nonzero\",\n";
    out << "    \"layoutRule\": \"a frame is 10:10:10:2 only when every pixel has alpha exactly 0 "
           "or the 8-bit alpha 255; status word 1 is nonzero otherwise and a 10:10:10:2 pack of "
           "such a frame must not be published\",\n";
    out << "    \"code10Rule\": \"floor(clamp(value,0,1)*1023+0.5) in binary32, in the order "
           "written, no contraction\",\n";
    out << "    \"counters\": \"display color times alpha, before any clamp: under and over by "
           "the 10-bit code rule per channel and for luma (when coefficients are pushed), and "
           "pixels whose signal is not finite; gathered per workgroup, one atomic per word\",\n";
    out << "    \"subnormalPolicy\": \"any nonzero subnormal input lane sets the whole frame's "
           "error flag; the GPU path rejects the frame and a CPU fallback is the intended route "
           "until denormal preservation is qualified\"\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

// OCIO reads several ambient environment variables directly (not through the explicit Context)
// during config validation and processor optimization. Clearing them for this process makes the
// generator's output independent of the host environment.
void neutralizeOcioEnvironment() {
    static constexpr const char* kAmbientOcioVariables[] = {
        "OCIO",
        "OCIO_ACTIVE_DISPLAYS",
        "OCIO_ACTIVE_VIEWS",
        "OCIO_INACTIVE_COLORSPACES",
        "OCIO_USER_CATEGORIES",
        "OCIO_OPTIMIZATION_FLAGS",
        "OCIO_LOGGING_LEVEL",
    };
    for (const char* const name : kAmbientOcioVariables) {
        OCIO::SetEnvVariable(name, nullptr);
    }
}

void printUsage() {
    std::cerr << "usage: bloom_gpu_neutral_display_generator --asset <config.ocio> "
                 "--output-dir <dir>\n";
}

} // namespace

int main(int argc, char** argv) {
    neutralizeOcioEnvironment();

    std::string assetPath;
    std::string outputDir;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--asset" && i + 1 < argc) {
            assetPath = argv[++i];
        } else if (argument == "--output-dir" && i + 1 < argc) {
            outputDir = argv[++i];
        } else if (argument == "--help") {
            printUsage();
            return 0;
        } else {
            std::cerr << "unrecognized argument: " << argument << "\n";
            printUsage();
            return 2;
        }
    }
    if (assetPath.empty() || outputDir.empty()) {
        printUsage();
        return 2;
    }

    try {
        const std::string payload = readFile(assetPath);

        const std::string rawDigest = sha256Hex(payload);
        if (rawDigest != BLOOM_NEUTRAL_V1_RAW_SHA256) {
            std::cerr << "refusing modified asset: raw SHA-256 " << rawDigest
                      << " does not match the pinned " << BLOOM_NEUTRAL_V1_RAW_SHA256 << "\n";
            return 3;
        }
        if (payload.size() != static_cast<std::size_t>(BLOOM_NEUTRAL_V1_PAYLOAD_BYTE_COUNT)) {
            std::cerr << "refusing asset with unexpected byte count\n";
            return 3;
        }
        const std::string revisionDigest = bloomOcioRevisionV1Hex(payload);
        if (revisionDigest != BLOOM_NEUTRAL_V1_REVISION_SHA256) {
            std::cerr << "refusing asset: BloomOcioRevision v1 digest " << revisionDigest
                      << " does not match the pinned " << BLOOM_NEUTRAL_V1_REVISION_SHA256 << "\n";
            return 3;
        }

        // Parse from the exact verified bytes; never from a path, environment, or search root.
        std::istringstream stream(payload);
        OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromStream(stream);
        if (!config) {
            std::cerr << "OCIO failed to parse the asset\n";
            return 4;
        }
        config->validate();

        const GpuShaderProducts products = extractGpuShader(config);
        const std::string ocioVersion = OCIO::GetVersion();
        const std::string shaderSource =
            buildShaderSource(products, rawDigest, revisionDigest, ocioVersion);
        const std::string shaderDigest = sha256Hex(shaderSource);
        const std::string manifest =
            buildManifest(products, rawDigest, revisionDigest, ocioVersion, shaderDigest);

        const std::filesystem::path outputRoot(outputDir);
        std::filesystem::create_directories(outputRoot);
        writeFile(outputRoot / "neutral_display.comp", shaderSource);
        writeFile(outputRoot / "neutral_display.manifest", manifest);

        std::cout << "rawSha256=" << rawDigest << "\n";
        std::cout << "revisionSha256=" << revisionDigest << "\n";
        std::cout << "ocioVersion=" << ocioVersion << "\n";
        std::cout << "processorCacheId=" << products.processorCacheId << "\n";
        std::cout << "shaderSha256=" << shaderDigest << "\n";
        std::cout << "wrote " << (outputRoot / "neutral_display.comp").string() << " and "
                  << (outputRoot / "neutral_display.manifest").string() << "\n";
    } catch (const std::exception& error) {
        std::cerr << "generator failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
