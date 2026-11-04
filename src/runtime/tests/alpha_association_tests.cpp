// #503 Alpha Association on the CPU reference: every mode against an independent double-precision
// formula over an HDR, signed, partial-alpha image that also holds zero, near-zero and
// exactly-epsilon alpha; the compositing identities each mode promises (Unmult over black and over
// white, Unpremultiply then Premultiply, Remove Color Matting of a matted pixel); finiteness, alpha
// range and transparent black at alpha 0; identity, domains, the one fixed program whose mode and
// background are data, the cache identity, the pixel budget and row cancellation.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::AlphaAssociationKernel;
using runtime::AlphaAssociationMode;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr float kEpsilon = document::kAlphaAssociationEpsilon;

// Premultiplied pixels: HDR fire on black (alpha 1, above one), smoke, a white-backed flare, signed
// colour, partial alpha, alpha 0, a tiny alpha, exactly epsilon and just above it.
const std::vector<std::array<float, 4>>& pixels() {
    static const std::vector<std::array<float, 4>> values{
        {6.0F, 2.5F, 0.4F, 1.0F},        {0.35F, 0.2F, 0.05F, 1.0F},
        {0.02F, 0.01F, 0.0F, 1.0F},      {1.0F, 0.9F, 0.6F, 1.0F},
        {1.4F, 1.2F, 1.1F, 1.0F},        {-0.25F, 0.5F, 0.125F, 0.75F},
        {0.3F, 0.15F, 0.6F, 0.6F},       {2.0F, 0.5F, 0.25F, 0.25F},
        {0.0F, 0.0F, 0.0F, 0.0F},        {3e-7F, 1e-7F, 2e-7F, 4e-7F},
        {5e-7F, 2e-7F, 9e-7F, kEpsilon}, {4e-6F, 1e-6F, -2e-6F, 5e-6F},
        {-0.5F, -0.25F, -1.0F, 1.0F}};
    return values;
}

render::Rgba32fImage image() {
    const auto count = static_cast<std::int64_t>(pixels().size());
    const auto data = render::ImageWindow::create(-3, 2, static_cast<std::uint32_t>(count), 1);
    const auto display = render::ImageWindow::create(0, 0, 16, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
    for (std::int64_t x = 0; x < count; ++x) {
        const auto& p = pixels()[static_cast<std::size_t>(x)];
        const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
        if (!pixel || builder.value()->write(x - 3, 2, *pixel.value()))
            throw std::logic_error("fixture pixel writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

AlphaAssociationKernel kernel(const AlphaAssociationMode mode, const bool white = false,
                              const core::Color4d background = {0.0, 0.0, 0.0, 1.0}) {
    return {mode, white, {document::ParameterId::fromRaw(7), background}};
}

std::optional<runtime::detail::PreparedColorGrade> prepare(const AlphaAssociationKernel& item) {
    const auto& color = std::get<core::Color4d>(item.background.source);
    return runtime::detail::prepareColorGrade(
        item, std::vector<double>{color.red, color.green, color.blue, color.alpha});
}

// The specification in double precision (docs/architecture/image-filters.md, "Alpha
// Association"), independent of the binary32 table.
std::array<double, 4> reference(const AlphaAssociationKernel& item,
                                const std::array<float, 4>& in) {
    const std::array<double, 4> p{static_cast<double>(in[0]), static_cast<double>(in[1]),
                                  static_cast<double>(in[2]), static_cast<double>(in[3])};
    const auto& b = std::get<core::Color4d>(item.background.source);
    std::array<double, 4> out = p;
    switch (item.mode) {
    case AlphaAssociationMode::Unmult:
        if (!item.white) {
            out[3] = std::clamp(std::max({p[0], p[1], p[2]}), 0.0, 1.0);
        } else {
            const std::array<double, 3> w{p[0] + 1.0 - p[3], p[1] + 1.0 - p[3], p[2] + 1.0 - p[3]};
            out[3] = std::clamp(1.0 - std::min({w[0], w[1], w[2]}), 0.0, 1.0);
            for (std::size_t c = 0; c < 3; ++c)
                out[c] = w[c] - (1.0 - out[3]);
        }
        break;
    case AlphaAssociationMode::Premultiply:
        for (std::size_t c = 0; c < 3; ++c)
            out[c] = p[c] * p[3];
        break;
    case AlphaAssociationMode::Unpremultiply:
        if (p[3] > static_cast<double>(kEpsilon))
            for (std::size_t c = 0; c < 3; ++c)
                out[c] = p[c] / p[3];
        break;
    case AlphaAssociationMode::RemoveMatting:
        out[0] = p[0] - (1.0 - p[3]) * b.red;
        out[1] = p[1] - (1.0 - p[3]) * b.green;
        out[2] = p[2] - (1.0 - p[3]) * b.blue;
        break;
    }
    if (out[3] == 0.0)
        out = {0.0, 0.0, 0.0, 0.0};
    return out;
}

std::vector<std::array<float, 4>> apply(const AlphaAssociationKernel& item) {
    const auto grade = prepare(item);
    expect(grade.has_value(), "the alpha association prepares");
    if (!grade)
        return {};
    const auto input = image();
    const auto result = runtime::detail::applyColorGrade(*grade, input, 1U << 22U, {},
                                                         runtime::OperationIndex::fromRaw(0), {});
    expect(result.image &&
               result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow() &&
               result.image->descriptor()->displayWindow() == input.descriptor()->displayWindow(),
           "the alpha association keeps the data and display windows");
    std::vector<std::array<float, 4>> out;
    if (!result.image)
        return out;
    for (std::int64_t x = 0; x < static_cast<std::int64_t>(pixels().size()); ++x) {
        const auto pixel = result.image->read(x - 3, 2);
        std::array<float, 4> value{};
        if (pixel)
            std::ranges::copy(pixel.value()->components(), value.begin());
        out.push_back(value);
    }
    return out;
}

template <typename Actual, typename Expected>
bool near(const Actual actualValue, const Expected expectedValue) {
    const auto actual = static_cast<double>(actualValue);
    const auto expected = static_cast<double>(expectedValue);
    return std::abs(actual - expected) <= 2e-6 * std::max(1.0, std::abs(expected));
}

void testModesAgainstReference() {
    const std::vector<std::pair<AlphaAssociationKernel, std::string>> cases{
        {kernel(AlphaAssociationMode::Unmult), "Unmult"},
        {kernel(AlphaAssociationMode::Unmult, true), "Unmult over white"},
        {kernel(AlphaAssociationMode::Premultiply), "Premultiply"},
        {kernel(AlphaAssociationMode::Unpremultiply), "Unpremultiply"},
        {kernel(AlphaAssociationMode::RemoveMatting, false, {0.2, 0.5, 1.5, 0.3}),
         "Remove Color Matting"}};
    for (const auto& [item, name] : cases) {
        const auto out = apply(item);
        if (out.size() != pixels().size())
            continue;
        bool matches = true;
        bool valid = true;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto expected = reference(item, pixels()[i]);
            for (std::size_t c = 0; c < 3; ++c)
                matches = matches && near(out[i][c], expected[c]);
            // Alpha is a max, a clamp or the input, so exact; over white it is one binary32
            // subtraction from one, so within rounding of one.
            matches = matches && (item.white ? near(out[i][3], expected[3])
                                             : static_cast<double>(out[i][3]) == expected[3]);
            for (const auto value : out[i])
                valid = valid && std::isfinite(value);
            valid = valid && out[i][3] >= 0.0F && out[i][3] <= 1.0F &&
                    (out[i][3] != 0.0F ||
                     (out[i][0] == 0.0F && out[i][1] == 0.0F && out[i][2] == 0.0F));
        }
        expect(matches,
               name +
                   " matches the double-precision reference (2e-6; exact alpha except over white)");
        expect(valid, name + " is finite, keeps alpha in [0, 1] and alpha 0 transparent black");
    }
}

void testCompositingIdentities() {
    // Unmult: the result over black is the input over black, HDR included (alpha clamps to one,
    // colour never does).
    const auto black = apply(kernel(AlphaAssociationMode::Unmult));
    bool overBlack = black.size() == pixels().size();
    for (std::size_t i = 0; overBlack && i < black.size(); ++i) {
        const auto& in = pixels()[i];
        const bool lit = std::max({in[0], in[1], in[2]}) > 0.0F;
        for (std::size_t c = 0; lit && c < 3; ++c)
            overBlack = overBlack && black[i][c] == in[c];
    }
    expect(overBlack, "Unmult over black reproduces the image over black exactly");
    expect(black.size() > 0 && black[0][3] == 1.0F && black[0][0] == 6.0F,
           "Unmult keeps HDR fire above one opaque with its colour unclamped");
    expect(black.size() > 2 && black[2][3] == 0.02F,
           "Unmult takes a dim pixel's alpha from its brightest channel");
    // Over white: rgb + (1 - alpha) is the input over white.
    const auto white = apply(kernel(AlphaAssociationMode::Unmult, true));
    bool overWhite = white.size() == pixels().size();
    for (std::size_t i = 0; overWhite && i < white.size(); ++i) {
        const auto& in = pixels()[i];
        if (white[i][3] == 0.0F)
            continue;
        for (std::size_t c = 0; c < 3; ++c)
            overWhite =
                overWhite &&
                near(static_cast<double>(white[i][c]) + 1.0 - static_cast<double>(white[i][3]),
                     static_cast<double>(in[c]) + 1.0 - static_cast<double>(in[3]));
    }
    expect(overWhite, "Unmult over white reproduces the image over white");
    expect(white.size() > 3 && white[3][3] == 1.0F - 0.6F,
           "a white-backed pixel's alpha is one minus its darkest channel over white");
    // Unpremultiply then Premultiply returns the input above epsilon; at or below it the colour
    // passes unchanged, never divided.
    const auto straight = apply(kernel(AlphaAssociationMode::Unpremultiply));
    bool roundTrip = straight.size() == pixels().size();
    for (std::size_t i = 0; roundTrip && i < straight.size(); ++i) {
        const auto& in = pixels()[i];
        for (std::size_t c = 0; c < 3; ++c)
            roundTrip =
                roundTrip &&
                (in[3] > kEpsilon
                     ? near(static_cast<double>(straight[i][c]) * static_cast<double>(in[3]), in[c])
                     : straight[i][c] == in[c]);
    }
    expect(roundTrip, "Unpremultiply inverts premultiplication above epsilon and leaves alpha "
                      "at or below epsilon undivided");
    expect(straight.size() > 11 && straight[10][0] == pixels()[10][0] && near(straight[11][0], 0.8),
           "exactly-epsilon alpha is not divided; just above it is");
    // Remove Color Matting: a pixel premultiplied against a colour, aF + (1 - a)B, becomes aF.
    const core::Color4d background{0.25, 0.75, 1.5, 1.0};
    const std::array<double, 3> foreground{1.2, 0.4, -0.1};
    const double a = 0.6;
    const auto matting = prepare(kernel(AlphaAssociationMode::RemoveMatting, false, background));
    std::array<float, 4> matted{static_cast<float>(a * foreground[0] + (1.0 - a) * 0.25),
                                static_cast<float>(a * foreground[1] + (1.0 - a) * 0.75),
                                static_cast<float>(a * foreground[2] + (1.0 - a) * 1.5),
                                static_cast<float>(a)};
    expect(matting && runtime::detail::colorGradePixel(*matting, matted) &&
               near(matted[0], a * foreground[0]) && near(matted[1], a * foreground[1]) &&
               near(matted[2], a * foreground[2]) && matted[3] == static_cast<float>(a),
           "Remove Color Matting recovers the colour premultiplied against the background");
    // Huge HDR above epsilon that cannot be represented divided is refused, never Inf.
    const auto unpremultiply = prepare(kernel(AlphaAssociationMode::Unpremultiply));
    std::array<float, 4> overflow{3e38F, 0.0F, 0.0F, 1e-5F};
    expect(unpremultiply && !runtime::detail::colorGradePixel(*unpremultiply, overflow),
           "a quotient beyond binary32 is reported as non-finite rather than published");
}

void testDomainsProgramAndIdentity() {
    expect(prepare(kernel(AlphaAssociationMode::RemoveMatting))->identity,
           "Remove Color Matting against black is the exact identity");
    for (const auto mode : {AlphaAssociationMode::Unmult, AlphaAssociationMode::Premultiply,
                            AlphaAssociationMode::Unpremultiply})
        expect(!prepare(kernel(mode))->identity, "every other mode does work");
    expect(prepare(kernel(AlphaAssociationMode::Unmult, false, {0.3, 0.2, 0.1, 1.0}))->data ==
               prepare(kernel(AlphaAssociationMode::Unmult))->data,
           "a background outside Remove Color Matting does not change the prepared table");
    expect(!prepare(kernel(AlphaAssociationMode::RemoveMatting, false, {2e6, 0.0, 0.0, 1.0})) &&
               !prepare(kernel(AlphaAssociationMode::RemoveMatting, false,
                               {std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0})) &&
               !runtime::detail::prepareColorGrade(kernel(AlphaAssociationMode::Unmult),
                                                   std::vector<double>{0.0, 0.0}) &&
               !runtime::detail::prepareColorGrade(
                   AlphaAssociationKernel{static_cast<AlphaAssociationMode>(4), false, {}},
                   std::vector<double>{0.0, 0.0, 0.0, 1.0}),
           "an out-of-domain background, operand count or mode is refused");
    std::vector<std::string> texts;
    std::vector<std::string> identities;
    for (const auto& item :
         {kernel(AlphaAssociationMode::Unmult), kernel(AlphaAssociationMode::Unmult, true),
          kernel(AlphaAssociationMode::Premultiply), kernel(AlphaAssociationMode::Unpremultiply),
          kernel(AlphaAssociationMode::RemoveMatting, false, {0.1, 0.2, 0.3, 1.0}),
          kernel(AlphaAssociationMode::RemoveMatting, false, {0.4, 0.2, 0.3, 1.0})}) {
        const auto grade = prepare(item);
        const auto program = runtime::detail::buildColorGradeProgram(grade->kind, grade->data);
        expect(program.succeeded() &&
                   render::validateOcioGpuProgram(*program.program(), {}) ==
                       render::OcioGpuProgramError::None &&
                   program.program()->semanticsId == "bloom.render.alpha-association.v1",
               "each mode builds the valid alpha association program");
        if (program.succeeded())
            texts.push_back(program.program()->shaderText);
        identities.push_back(runtime::detail::colorGradeIdentity(*grade));
    }
    expect(texts.size() == 6 &&
               std::ranges::all_of(texts, [&](const auto& text) { return text == texts.front(); }),
           "every mode and background runs one fixed program: they are data");
    std::ranges::sort(identities);
    expect(std::ranges::adjacent_find(identities) == identities.end(),
           "each mode and background has its own cache identity");
    const auto grade = prepare(kernel(AlphaAssociationMode::Unmult));
    auto malformed = grade->data;
    malformed[0] = 9.0F;
    expect(!runtime::detail::buildColorGradeProgram(grade->kind, {0.0F, 0.0F}).succeeded() &&
               !runtime::detail::buildColorGradeProgram(grade->kind, malformed).succeeded(),
           "a malformed alpha association table is refused");
}

void testBudgetAndCancellation() {
    const auto input = image();
    const auto grade = prepare(kernel(AlphaAssociationMode::Unpremultiply));
    const auto refused = runtime::detail::applyColorGrade(*grade, input, 64, {},
                                                          runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "an alpha association beyond the pixel budget is refused");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(503)};
    bool cancelled = false;
    auto submission =
        scheduler.submit<void>(runtime::TaskRequest("Alpha association cancellation", owner),
                               [&](runtime::TaskContext& context) {
                                   static_cast<void>(scheduler.cancelOwner(owner));
                                   const auto result = runtime::detail::applyColorGrade(
                                       *grade, input, 1U << 22U, context.cancellation(),
                                       runtime::OperationIndex::fromRaw(0), {});
                                   cancelled = result.cancelled && !result.image;
                                   return runtime::TaskResult<void>::cancelled();
                               });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(result && result->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled alpha association publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled alpha association shuts down safely");
}

} // namespace

int main() {
    try {
        testModesAgainstReference();
        testCompositingIdentities();
        testDomainsProgramAndIdentity();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " alpha association expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: alpha association modes, identities, domains, program, budget and "
                 "cancellation\n";
    return 0;
}
