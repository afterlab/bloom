// #724, #730 Scopes: the panel's side of the count-to-image mapping (ui/scopes_drawing.hpp).
//
// Asserted, over known tables of a few pixels: the panel's trace image is the Qt-free mapping's
// (runtime/scope_picture.hpp, whose drawing scope_picture_tests asserts), as a premultiplied
// QImage; the light traces are drawn additively; a scope the tables lack, a cancelled mapping and
// a request without tables give a null image. No widget, device or event loop is used.

#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/ui/scopes_drawing.hpp>

#include <QColor>
#include <QCoreApplication>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace scopes = bloom::ui::scopes;
namespace ui = bloom::ui;
using render::Rgba32f;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] Rgba32f px(const float r, const float g, const float b, const float a = 1.0F) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!result)
        throw std::logic_error("invalid fixture pixel");
    return *result.value();
}

[[nodiscard]] runtime::ScopeTables tablesOf(const std::vector<Rgba32f>& signal,
                                            const std::uint32_t width, const std::uint32_t height,
                                            const std::uint32_t scopeBits,
                                            const std::vector<Rgba32f>& xyz = {}) {
    runtime::ScopeMeasureInput input;
    input.signal = signal;
    input.xyz = xyz;
    input.width = width;
    input.height = height;
    input.areaWidth = width;
    input.areaHeight = height;
    input.scopes = scopeBits;
    input.axisMaximum = 1;
    input.luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    const auto counts = runtime::measureScopesCpu(input);
    if (!counts)
        throw std::logic_error("the fixture could not be measured");
    return runtime::ScopeTables::pack(*counts, runtime::ScopeRoute::Cpu);
}

void testToQImage(Expectations& e) {
    runtime::scopes::ScopeImage image;
    image.width = 2;
    image.height = 1;
    image.pixels = {runtime::scopes::packArgb(255, 255, 0, 0),
                    runtime::scopes::packArgb(128, 0, 64, 0)};
    const auto converted = scopes::toQImage(image);
    e.expect(converted.format() == QImage::Format_ARGB32_Premultiplied &&
                 converted.size() == QSize(2, 1) && converted.pixel(0, 0) == qRgba(255, 0, 0, 255),
             "a mapping buffer becomes a premultiplied QImage of its pixels");
    e.expect(scopes::toQImage({}).isNull(), "an empty buffer is a null image");
}

void testAdditive(Expectations& e) {
    e.expect(scopes::additiveTrace(scopes::Kind::Waveform) &&
                 scopes::additiveTrace(scopes::Kind::Parade) &&
                 scopes::additiveTrace(scopes::Kind::Vectorscope) &&
                 !scopes::additiveTrace(scopes::Kind::Histogram) &&
                 !scopes::additiveTrace(scopes::Kind::Chromaticity),
             "the light traces add; the Histogram and the CIE cover");
}

void testRenderTrace(Expectations& e) {
    std::vector<Rgba32f> pixels;
    for (int x = 0; x < 64; ++x)
        pixels.push_back(
            px(static_cast<float>(x) / 63.0F, 0.5F, 1.0F - static_cast<float>(x) / 63.0F));
    auto tables = std::make_shared<const runtime::ScopeTables>(
        tablesOf(pixels, 64, 1, render::kScopeTableHistogram | render::kScopeTableWaveform));
    scopes::TraceRequest request;
    request.tables = tables;
    // The panel's image is the Qt-free mapping's, pixel for pixel.
    for (const auto kind :
         {scopes::Kind::Waveform, scopes::Kind::Parade, scopes::Kind::Histogram}) {
        request.kind = kind;
        request.size = kind == scopes::Kind::Histogram ? QSize(128, 20) : QSize(100, 60);
        const auto image = scopes::renderTrace(request);
        runtime::scopes::TraceRequest mapped;
        mapped.kind = kind;
        mapped.tables = tables.get();
        mapped.size = {request.size.width(), request.size.height()};
        mapped.gapPixels = request.gapPixels;
        const auto expected = scopes::toQImage(runtime::scopes::renderTrace(mapped));
        e.expect(!image.isNull() && image == expected, "the panel's trace is the mapping's (kind " +
                                                           std::to_string(static_cast<int>(kind)) +
                                                           ")");
    }
    request.kind = scopes::Kind::Histogram;
    request.size = {64, 8};
    e.expect(scopes::renderTrace(request).size() == QSize(64, 24), "three rows of the Histogram");
    request.kind = scopes::Kind::Vectorscope;
    e.expect(scopes::renderTrace(request).isNull(),
             "a scope the tables do not hold maps to nothing");
    e.expect(scopes::renderTrace(request, [] { return true; }).isNull(),
             "a cancelled mapping returns nothing");
    request.kind = scopes::Kind::Waveform;
    e.expect(scopes::renderTrace(request, [] { return true; }).isNull(),
             "a cancelled Waveform returns nothing");
    scopes::TraceRequest empty;
    e.expect(scopes::renderTrace(empty).isNull(), "a request without tables is empty");
}

} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication application(argc, argv);
        Expectations expectations;
        testToQImage(expectations);
        testAdditive(expectations);
        testRenderTrace(expectations);
        if (expectations.failures() != 0)
            return 1;
        std::cout << "PASS: scopes drawing\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
