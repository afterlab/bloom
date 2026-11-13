#include <bloom/runtime/variable_font_instance.hpp>

#include "variable_font_tables.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

// A variable TrueType font is a default outline set plus per-glyph deltas (OpenType `gvar`) that a
// point in the design space scales and adds. This file applies one point and writes the result back
// as a static sfnt, which the unhinted glyph source then reads like any other font file. The
// formats are the public OpenType tables: fvar, avar, gvar (with intermediate regions, shared and
// private point numbers and interpolation of untouched points), HVAR (an item variation store and
// delta-set index maps), glyf, loca, hmtx, and cvar. Hinting is kept: the glyphs' programs and
// `fpgm` and `prep` are carried as they are and the control values follow the location (#489).
// A font inside a collection is read through the collection's directory, and a font with CFF2
// outlines goes to variable_font_cff2.cpp.

namespace bloom::runtime::fonts {

namespace {

using namespace detail;

constexpr std::size_t kMaximumGlyphs = 65535;
constexpr int kMaximumComponentDepth = 8;

struct Axis final {
    std::uint32_t tag = 0;
    double minimum = 0.0;
    double defaultValue = 0.0;
    double maximum = 0.0;
};

struct Fvar final {
    std::vector<Axis> axes;
    // Each named instance's user-space coordinates, in axis order.
    std::vector<std::vector<double>> instances;
};

[[nodiscard]] std::optional<Fvar> parseFvar(const std::span<const std::uint8_t> table) {
    if (table.empty())
        return std::nullopt;
    Reader reader(table);
    reader.skip(4);
    const auto axesOffset = reader.u16();
    reader.skip(2);
    const auto axisCount = reader.u16();
    const auto axisSize = reader.u16();
    const auto instanceCount = reader.u16();
    const auto instanceSize = reader.u16();
    if (reader.failed() || axisCount == 0 || axisCount > 64 || axisSize < 20)
        return std::nullopt;
    Fvar fvar;
    for (std::uint16_t i = 0; i < axisCount; ++i) {
        Reader record(table, static_cast<std::size_t>(axesOffset) + static_cast<std::size_t>(i) * axisSize);
        Axis axis;
        axis.tag = record.u32();
        axis.minimum = record.fixed();
        axis.defaultValue = record.fixed();
        axis.maximum = record.fixed();
        if (record.failed())
            return std::nullopt;
        fvar.axes.push_back(axis);
    }
    const auto instancesStart =
        static_cast<std::size_t>(axesOffset) + static_cast<std::size_t>(axisCount) * axisSize;
    for (std::uint16_t i = 0; i < instanceCount; ++i) {
        Reader record(table, instancesStart + static_cast<std::size_t>(i) * instanceSize + 4);
        std::vector<double> coordinates;
        for (std::uint16_t a = 0; a < axisCount; ++a)
            coordinates.push_back(record.fixed());
        if (record.failed())
            break;
        fvar.instances.push_back(std::move(coordinates));
    }
    return fvar;
}

// avar version 1: a piecewise-linear remap of each axis's normalized coordinate.
using SegmentMap = std::vector<std::pair<double, double>>;

[[nodiscard]] std::vector<SegmentMap> parseAvar(const std::span<const std::uint8_t> table,
                                                const std::size_t axisCount) {
    std::vector<SegmentMap> maps;
    if (table.empty())
        return maps;
    Reader reader(table);
    reader.skip(6);
    const auto count = reader.u16();
    if (count != axisCount)
        return {};
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto pairs = reader.u16();
        SegmentMap map;
        for (std::uint16_t p = 0; p < pairs; ++p) {
            const auto from = reader.f2dot14();
            const auto to = reader.f2dot14();
            map.emplace_back(from, to);
        }
        maps.push_back(std::move(map));
    }
    return reader.failed() ? std::vector<SegmentMap>{} : maps;
}

[[nodiscard]] double applySegmentMap(const SegmentMap& map, const double value) {
    if (map.size() < 2)
        return value;
    if (value <= map.front().first)
        return map.front().second + (value - map.front().first);
    for (std::size_t i = 1; i < map.size(); ++i) {
        if (value <= map[i].first) {
            const auto [fromA, toA] = map[i - 1];
            const auto [fromB, toB] = map[i];
            if (fromB == fromA)
                return toB;
            return toA + (toB - toA) * (value - fromA) / (fromB - fromA);
        }
    }
    return map.back().second + (value - map.back().first);
}

[[nodiscard]] std::vector<double> normalize(const Fvar& fvar, const std::vector<double>& user,
                                            const std::vector<SegmentMap>& avar) {
    std::vector<double> out(fvar.axes.size(), 0.0);
    for (std::size_t i = 0; i < fvar.axes.size(); ++i) {
        const auto& axis = fvar.axes[i];
        const auto value = std::clamp(user[i], axis.minimum, axis.maximum);
        double n = 0.0;
        if (value < axis.defaultValue && axis.defaultValue > axis.minimum)
            n = (value - axis.defaultValue) / (axis.defaultValue - axis.minimum);
        else if (value > axis.defaultValue && axis.maximum > axis.defaultValue)
            n = (value - axis.defaultValue) / (axis.maximum - axis.defaultValue);
        n = std::clamp(n, -1.0, 1.0);
        if (i < avar.size())
            n = std::clamp(applySegmentMap(avar[i], n), -1.0, 1.0);
        out[i] = n;
    }
    return out;
}

// ---- gvar -------------------------------------------------------------------------------------

struct Point final {
    int x = 0;
    int y = 0;
};

[[nodiscard]] std::optional<std::vector<std::uint32_t>> packedPointNumbers(Reader& reader,
                                                                          const std::size_t total) {
    // An empty vector means "every point".
    std::vector<std::uint32_t> points;
    auto count = static_cast<std::size_t>(reader.u8());
    if (count == 0)
        return points;
    if ((count & 0x80U) != 0)
        count = ((count & 0x7FU) << 8U) | reader.u8();
    std::uint32_t last = 0;
    while (points.size() < count) {
        const auto control = reader.u8();
        const auto run = static_cast<std::size_t>(control & 0x7FU) + 1U;
        const bool words = (control & 0x80U) != 0;
        for (std::size_t i = 0; i < run && points.size() < count; ++i) {
            last += words ? reader.u16() : reader.u8();
            if (last >= total)
                return std::nullopt;
            points.push_back(last);
        }
        if (reader.failed())
            return std::nullopt;
    }
    return points;
}

[[nodiscard]] std::optional<std::vector<int>> packedDeltas(Reader& reader, const std::size_t count) {
    std::vector<int> deltas;
    deltas.reserve(count);
    while (deltas.size() < count) {
        const auto control = reader.u8();
        const auto run = static_cast<std::size_t>(control & 0x3FU) + 1U;
        for (std::size_t i = 0; i < run && deltas.size() < count; ++i) {
            if ((control & 0x80U) != 0)
                deltas.push_back(0);
            else if ((control & 0x40U) != 0)
                deltas.push_back(reader.i16());
            else
                deltas.push_back(reader.i8());
        }
        if (reader.failed())
            return std::nullopt;
    }
    return deltas;
}

// Interpolates the deltas of untouched points of one axis between the touched ones (OpenType
// "inferred deltas"): a point between two touched points in coordinate order takes a linear blend
// of their deltas, one outside takes the nearest touched delta.
void interpolateUntouched(const std::vector<int>& original, std::vector<double>& deltas,
                          const std::vector<bool>& touched, const std::size_t first,
                          const std::size_t last) {
    std::vector<std::size_t> anchors;
    for (std::size_t i = first; i <= last; ++i)
        if (touched[i])
            anchors.push_back(i);
    if (anchors.empty())
        return;
    const auto size = last - first + 1;
    if (anchors.size() == 1) {
        for (std::size_t i = first; i <= last; ++i)
            deltas[i] = deltas[anchors.front()];
        return;
    }
    for (std::size_t a = 0; a < anchors.size(); ++a) {
        const auto from = anchors[a];
        const auto to = anchors[(a + 1) % anchors.size()];
        // The untouched points strictly after `from` and before `to`, wrapping in the contour.
        auto rc1 = static_cast<double>(original[from]);
        auto rc2 = static_cast<double>(original[to]);
        auto rd1 = deltas[from];
        auto rd2 = deltas[to];
        if (rc1 > rc2) {
            std::swap(rc1, rc2);
            std::swap(rd1, rd2);
        }
        for (std::size_t i = (from - first + 1) % size; (i + first) != to; i = (i + 1) % size) {
            const auto p = first + i;
            const auto c = static_cast<double>(original[p]);
            double d = 0.0;
            if (rc1 == rc2)
                d = rd1 == rd2 ? rd1 : 0.0;
            else if (c <= rc1)
                d = rd1;
            else if (c >= rc2)
                d = rd2;
            else
                d = rd1 + (c - rc1) * (rd2 - rd1) / (rc2 - rc1);
            deltas[p] = d;
        }
    }
}

struct Gvar final {
    std::span<const std::uint8_t> table;
    std::size_t axisCount = 0;
    std::size_t glyphCount = 0;
    bool longOffsets = false;
    std::size_t dataArrayOffset = 0;
    std::size_t offsetsPosition = 0;
    std::vector<std::vector<double>> sharedTuples;
};

[[nodiscard]] std::optional<Gvar> parseGvar(const std::span<const std::uint8_t> table,
                                            const std::size_t axisCount) {
    Reader reader(table);
    reader.skip(4);
    Gvar gvar;
    gvar.table = table;
    gvar.axisCount = reader.u16();
    const auto sharedCount = reader.u16();
    const auto sharedOffset = reader.u32();
    gvar.glyphCount = reader.u16();
    gvar.longOffsets = (reader.u16() & 1U) != 0;
    gvar.dataArrayOffset = reader.u32();
    gvar.offsetsPosition = reader.position();
    if (reader.failed() || gvar.axisCount != axisCount)
        return std::nullopt;
    Reader shared(table, sharedOffset);
    for (std::uint16_t i = 0; i < sharedCount; ++i) {
        std::vector<double> tuple;
        for (std::size_t a = 0; a < axisCount; ++a)
            tuple.push_back(shared.f2dot14());
        gvar.sharedTuples.push_back(std::move(tuple));
    }
    if (shared.failed())
        return std::nullopt;
    return gvar;
}

// The glyph's variation data, empty when it has none.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> gvarGlyphData(const Gvar& gvar,
                                                                        const std::uint32_t glyph) {
    if (glyph >= gvar.glyphCount)
        return std::span<const std::uint8_t>{};
    Reader reader(gvar.table, gvar.offsetsPosition +
                                  static_cast<std::size_t>(glyph) * (gvar.longOffsets ? 4U : 2U));
    std::size_t begin = 0;
    std::size_t end = 0;
    if (gvar.longOffsets) {
        begin = reader.u32();
        end = reader.u32();
    } else {
        begin = static_cast<std::size_t>(reader.u16()) * 2U;
        end = static_cast<std::size_t>(reader.u16()) * 2U;
    }
    if (reader.failed() || end < begin ||
        gvar.dataArrayOffset + end > gvar.table.size())
        return std::nullopt;
    return gvar.table.subspan(gvar.dataArrayOffset + begin, end - begin);
}

// ---- glyf -------------------------------------------------------------------------------------

struct Glyph final {
    // Simple glyph: the points, contour ends and on-curve flags. Composite: no points.
    std::vector<Point> points;
    std::vector<std::uint16_t> contourEnds;
    std::vector<bool> onCurve;
    bool composite = false;
    struct Component final {
        std::uint16_t flags = 0;
        std::uint16_t glyph = 0;
        int dx = 0;
        int dy = 0;
        // The scale or 2x2 bytes, copied through unchanged.
        std::vector<std::uint8_t> transform;
        double a = 1.0;
        double b = 0.0;
        double c = 0.0;
        double d = 1.0;
    };
    std::vector<Component> components;
    // The glyph's hinting program, carried into the instance as it is.
    std::vector<std::uint8_t> instructions;
    int xMin = 0;
    int yMin = 0;
    int xMax = 0;
    int yMax = 0;
};

constexpr std::uint16_t kArgsAreWords = 0x0001;
constexpr std::uint16_t kArgsAreXy = 0x0002;
constexpr std::uint16_t kHaveScale = 0x0008;
constexpr std::uint16_t kMoreComponents = 0x0020;
constexpr std::uint16_t kHaveXyScale = 0x0040;
constexpr std::uint16_t kHave2x2 = 0x0080;
constexpr std::uint16_t kHaveInstructions = 0x0100;

[[nodiscard]] std::optional<Glyph> parseGlyph(const std::span<const std::uint8_t> data) {
    Glyph glyph;
    if (data.empty())
        return glyph;
    Reader reader(data);
    const auto contours = reader.i16();
    glyph.xMin = reader.i16();
    glyph.yMin = reader.i16();
    glyph.xMax = reader.i16();
    glyph.yMax = reader.i16();
    if (reader.failed())
        return std::nullopt;
    if (contours >= 0) {
        for (std::int16_t i = 0; i < contours; ++i)
            glyph.contourEnds.push_back(reader.u16());
        const auto instructionLength = reader.u16();
        if (reader.failed() || instructionLength > data.size() - reader.position())
            return std::nullopt;
        glyph.instructions.assign(
            data.begin() + static_cast<std::ptrdiff_t>(reader.position()),
            data.begin() + static_cast<std::ptrdiff_t>(reader.position() + instructionLength));
        reader.skip(instructionLength);
        const std::size_t count = contours == 0 ? 0 : static_cast<std::size_t>(glyph.contourEnds.back()) + 1U;
        if (reader.failed() || count > kMaximumGlyphs)
            return std::nullopt;
        std::vector<std::uint8_t> flags;
        while (flags.size() < count) {
            const auto flag = reader.u8();
            flags.push_back(flag);
            if ((flag & 0x08U) != 0) {
                auto repeat = reader.u8();
                while (repeat-- > 0 && flags.size() < count)
                    flags.push_back(flag);
            }
            if (reader.failed())
                return std::nullopt;
        }
        glyph.points.assign(count, {});
        glyph.onCurve.assign(count, false);
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto flag = flags[i];
            if ((flag & 0x02U) != 0) {
                const int step = reader.u8();
                value += (flag & 0x10U) != 0 ? step : -step;
            } else if ((flag & 0x10U) == 0) {
                value += reader.i16();
            }
            glyph.points[i].x = value;
            glyph.onCurve[i] = (flag & 0x01U) != 0;
        }
        value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto flag = flags[i];
            if ((flag & 0x04U) != 0) {
                const int step = reader.u8();
                value += (flag & 0x20U) != 0 ? step : -step;
            } else if ((flag & 0x20U) == 0) {
                value += reader.i16();
            }
            glyph.points[i].y = value;
        }
        if (reader.failed())
            return std::nullopt;
        return glyph;
    }
    glyph.composite = true;
    while (true) {
        Glyph::Component component;
        component.flags = reader.u16();
        component.glyph = reader.u16();
        if ((component.flags & kArgsAreWords) != 0) {
            component.dx = (component.flags & kArgsAreXy) != 0 ? reader.i16() : reader.u16();
            component.dy = (component.flags & kArgsAreXy) != 0 ? reader.i16() : reader.u16();
        } else {
            component.dx = (component.flags & kArgsAreXy) != 0 ? reader.i8() : reader.u8();
            component.dy = (component.flags & kArgsAreXy) != 0 ? reader.i8() : reader.u8();
        }
        const auto transformStart = reader.position();
        if ((component.flags & kHaveScale) != 0) {
            component.a = component.d = reader.f2dot14();
        } else if ((component.flags & kHaveXyScale) != 0) {
            component.a = reader.f2dot14();
            component.d = reader.f2dot14();
        } else if ((component.flags & kHave2x2) != 0) {
            component.a = reader.f2dot14();
            component.b = reader.f2dot14();
            component.c = reader.f2dot14();
            component.d = reader.f2dot14();
        }
        if (reader.failed() || data.size() < reader.position())
            return std::nullopt;
        component.transform.assign(data.begin() + static_cast<std::ptrdiff_t>(transformStart),
                                   data.begin() + static_cast<std::ptrdiff_t>(reader.position()));
        const auto more = (component.flags & kMoreComponents) != 0;
        const bool instructed = (component.flags & kHaveInstructions) != 0;
        glyph.components.push_back(std::move(component));
        if (!more) {
            // A composite's hinting program follows its last component.
            if (instructed) {
                const auto length = reader.u16();
                if (reader.failed() || length > data.size() - reader.position())
                    return std::nullopt;
                glyph.instructions.assign(
                    data.begin() + static_cast<std::ptrdiff_t>(reader.position()),
                    data.begin() + static_cast<std::ptrdiff_t>(reader.position() + length));
            }
            break;
        }
        if (glyph.components.size() > 256)
            return std::nullopt;
    }
    return glyph;
}

struct Metrics final {
    int advance = 0;
    int leftBearing = 0;
};

struct Output final {
    std::vector<std::uint8_t> data;
    int advance = 0;
    int xMin = 0;
    int yMin = 0;
    int xMax = 0;
    int yMax = 0;
    bool empty = true;
};

// ---- cvar -------------------------------------------------------------------------------------

// The control values (`cvt`) at a location: every tuple of `cvar` in effect adds its scaled deltas
// to the values it lists, so the hinting programs that read them fit the instance. Null when the
// table is damaged.
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
variedControlValues(const std::span<const std::uint8_t> cvt, const std::span<const std::uint8_t> cvar,
                    const std::size_t axisCount, const std::vector<double>& location) {
    const std::size_t total = cvt.size() / 2U;
    Reader reader(cvar);
    reader.skip(4);
    const auto countField = reader.u16();
    const auto dataOffset = reader.u16();
    const auto tupleCount = static_cast<std::size_t>(countField & 0x0FFFU);
    if (reader.failed() || dataOffset > cvar.size())
        return std::nullopt;
    struct Header final {
        std::size_t size = 0;
        std::uint16_t index = 0;
        std::vector<double> start;
        std::vector<double> peak;
        std::vector<double> end;
    };
    std::vector<Header> headers;
    for (std::size_t t = 0; t < tupleCount; ++t) {
        Header header;
        header.size = reader.u16();
        header.index = reader.u16();
        // A control value tuple carries its own peak: the table has no shared tuples.
        if ((header.index & 0x8000U) == 0)
            return std::nullopt;
        for (std::size_t a = 0; a < axisCount; ++a)
            header.peak.push_back(reader.f2dot14());
        if ((header.index & 0x4000U) != 0) {
            for (std::size_t a = 0; a < axisCount; ++a)
                header.start.push_back(reader.f2dot14());
            for (std::size_t a = 0; a < axisCount; ++a)
                header.end.push_back(reader.f2dot14());
        } else {
            for (std::size_t a = 0; a < axisCount; ++a) {
                header.start.push_back(std::min(0.0, header.peak[a]));
                header.end.push_back(std::max(0.0, header.peak[a]));
            }
        }
        if (reader.failed())
            return std::nullopt;
        headers.push_back(std::move(header));
    }
    Reader serialized(cvar, dataOffset);
    std::vector<std::uint32_t> shared;
    bool sharedAll = true;
    if ((countField & 0x8000U) != 0) {
        auto points = packedPointNumbers(serialized, total);
        if (!points)
            return std::nullopt;
        shared = std::move(*points);
        sharedAll = shared.empty();
    }
    std::vector<double> deltas(total, 0.0);
    for (const auto& header : headers) {
        const auto scalar = regionScalar(location, header.start, header.peak, header.end);
        Reader tuple(cvar, serialized.position());
        const auto next = serialized.position() + header.size;
        if (next > cvar.size())
            return std::nullopt;
        std::vector<std::uint32_t> points = shared;
        bool all = sharedAll;
        if ((header.index & 0x2000U) != 0) {
            auto own = packedPointNumbers(tuple, total);
            if (!own)
                return std::nullopt;
            points = std::move(*own);
            all = points.empty();
        }
        serialized.seek(next);
        if (scalar == 0.0)
            continue;
        const auto listed = all ? total : points.size();
        const auto values = packedDeltas(tuple, listed);
        if (!values)
            return std::nullopt;
        for (std::size_t i = 0; i < listed; ++i)
            deltas[all ? i : points[i]] += (*values)[i] * scalar;
    }
    std::vector<std::uint8_t> varied;
    varied.reserve(cvt.size());
    Reader values(cvt);
    for (std::size_t i = 0; i < total; ++i)
        putS16(varied, values.i16() + static_cast<int>(std::lround(deltas[i])));
    return varied;
}

class Instancer final {
  public:
    Instancer(const Sfnt& sfnt, std::vector<double> location)
        : sfnt_(sfnt), location_(std::move(location)) {}

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> run(const Fvar& fvar) {
        const auto head = sfnt_.table(makeTag("head"));
        const auto maxp = sfnt_.table(makeTag("maxp"));
        const auto hhea = sfnt_.table(makeTag("hhea"));
        hmtx_ = sfnt_.table(makeTag("hmtx"));
        loca_ = sfnt_.table(makeTag("loca"));
        glyf_ = sfnt_.table(makeTag("glyf"));
        if (head.size() < 54 || maxp.size() < 6 || hhea.size() < 36 || hmtx_.empty() ||
            loca_.empty() || glyf_.empty())
            return std::nullopt;
        Reader headReader(head, 50);
        longLoca_ = headReader.i16() != 0;
        Reader maxpReader(maxp, 4);
        glyphCount_ = maxpReader.u16();
        Reader hheaReader(hhea, 34);
        metricCount_ = hheaReader.u16();
        if (glyphCount_ == 0 || metricCount_ == 0 || metricCount_ > glyphCount_ ||
            hmtx_.size() < static_cast<std::size_t>(metricCount_) * 4U +
                               static_cast<std::size_t>(glyphCount_ - metricCount_) * 2U)
            return std::nullopt;
        const auto gvarTable = sfnt_.table(makeTag("gvar"));
        const auto gvar = parseGvar(gvarTable, fvar.axes.size());
        if (!gvar)
            return std::nullopt;
        gvar_ = *gvar;
        if (const auto hvarTable = sfnt_.table(makeTag("HVAR")); !hvarTable.empty()) {
            Reader hv(hvarTable, 4);
            Hvar parsed;
            parsed.table = hvarTable;
            parsed.storeOffset = hv.u32();
            parsed.advanceMapOffset = hv.u32();
            if (!hv.failed())
                hvar_ = parsed;
        }
        outputs_.assign(glyphCount_, std::nullopt);
        for (std::uint32_t g = 0; g < glyphCount_; ++g)
            if (!process(g, 0))
                return std::nullopt;
        return rebuild();
    }

  private:
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> glyphBytes(const std::uint32_t glyph) const {
        Reader reader(loca_, static_cast<std::size_t>(glyph) * (longLoca_ ? 4U : 2U));
        std::size_t begin = 0;
        std::size_t end = 0;
        if (longLoca_) {
            begin = reader.u32();
            end = reader.u32();
        } else {
            begin = static_cast<std::size_t>(reader.u16()) * 2U;
            end = static_cast<std::size_t>(reader.u16()) * 2U;
        }
        if (reader.failed() || end < begin || end > glyf_.size())
            return std::nullopt;
        return glyf_.subspan(begin, end - begin);
    }

    [[nodiscard]] Metrics metricsFor(const std::uint32_t glyph) const {
        Metrics metrics;
        if (glyph < metricCount_) {
            Reader reader(hmtx_, static_cast<std::size_t>(glyph) * 4U);
            metrics.advance = reader.u16();
            metrics.leftBearing = reader.i16();
        } else {
            Reader advance(hmtx_, static_cast<std::size_t>(metricCount_ - 1U) * 4U);
            metrics.advance = advance.u16();
            Reader bearing(hmtx_, static_cast<std::size_t>(metricCount_) * 4U +
                                      static_cast<std::size_t>(glyph - metricCount_) * 2U);
            metrics.leftBearing = bearing.i16();
        }
        return metrics;
    }

    // The summed, scaled deltas of every tuple in effect, one entry per point plus four phantom
    // points. `pointCount` is the outline's point count (a composite's component count).
    [[nodiscard]] bool deltasFor(const std::uint32_t glyph, const Glyph& parsed, const std::size_t pointCount,
                                 std::vector<double>& dx, std::vector<double>& dy) const {
        const auto total = pointCount + 4;
        dx.assign(total, 0.0);
        dy.assign(total, 0.0);
        const auto data = gvarGlyphData(gvar_, glyph);
        if (!data)
            return false;
        if (data->empty())
            return true;
        Reader reader(*data);
        const auto countField = reader.u16();
        const auto dataOffset = reader.u16();
        const auto tupleCount = static_cast<std::size_t>(countField & 0x0FFFU);
        const bool sharedPoints = (countField & 0x8000U) != 0;
        if (reader.failed() || dataOffset > data->size())
            return false;

        struct Header final {
            std::size_t size = 0;
            std::uint16_t index = 0;
            std::vector<double> start;
            std::vector<double> peak;
            std::vector<double> end;
        };
        std::vector<Header> headers;
        const auto axisCount = gvar_.axisCount;
        for (std::size_t t = 0; t < tupleCount; ++t) {
            Header header;
            header.size = reader.u16();
            header.index = reader.u16();
            if ((header.index & 0x8000U) != 0) {
                for (std::size_t a = 0; a < axisCount; ++a)
                    header.peak.push_back(reader.f2dot14());
            } else {
                const auto shared = static_cast<std::size_t>(header.index & 0x0FFFU);
                if (shared >= gvar_.sharedTuples.size())
                    return false;
                header.peak = gvar_.sharedTuples[shared];
            }
            if ((header.index & 0x4000U) != 0) {
                for (std::size_t a = 0; a < axisCount; ++a)
                    header.start.push_back(reader.f2dot14());
                for (std::size_t a = 0; a < axisCount; ++a)
                    header.end.push_back(reader.f2dot14());
            } else {
                for (std::size_t a = 0; a < axisCount; ++a) {
                    header.start.push_back(std::min(0.0, header.peak[a]));
                    header.end.push_back(std::max(0.0, header.peak[a]));
                }
            }
            if (reader.failed())
                return false;
            headers.push_back(std::move(header));
        }

        Reader serialized(*data, dataOffset);
        std::vector<std::uint32_t> shared;
        bool sharedAll = true;
        if (sharedPoints) {
            auto points = packedPointNumbers(serialized, total);
            if (!points)
                return false;
            shared = std::move(*points);
            sharedAll = shared.empty();
        }

        std::vector<int> originalX(pointCount);
        std::vector<int> originalY(pointCount);
        for (std::size_t i = 0; i < parsed.points.size() && i < pointCount; ++i) {
            originalX[i] = parsed.points[i].x;
            originalY[i] = parsed.points[i].y;
        }

        for (const auto& header : headers) {
            const auto scalar = regionScalar(location_, header.start, header.peak, header.end);
            Reader tuple(*data, serialized.position());
            const auto next = serialized.position() + header.size;
            if (next > data->size())
                return false;
            std::vector<std::uint32_t> points;
            bool all = true;
            if ((header.index & 0x2000U) != 0) {
                auto parsedPoints = packedPointNumbers(tuple, total);
                if (!parsedPoints)
                    return false;
                points = std::move(*parsedPoints);
                all = points.empty();
            } else {
                points = shared;
                all = sharedAll;
            }
            serialized.seek(next);
            if (scalar == 0.0)
                continue;
            const auto listed = all ? total : points.size();
            auto xs = packedDeltas(tuple, listed);
            if (!xs)
                return false;
            auto ys = packedDeltas(tuple, listed);
            if (!ys)
                return false;
            std::vector<double> tdx(total, 0.0);
            std::vector<double> tdy(total, 0.0);
            std::vector<bool> touched(total, false);
            for (std::size_t i = 0; i < listed; ++i) {
                const auto p = all ? i : points[i];
                tdx[p] = (*xs)[i];
                tdy[p] = (*ys)[i];
                touched[p] = true;
            }
            if (!all && !parsed.composite) {
                std::size_t first = 0;
                for (const auto end : parsed.contourEnds) {
                    if (end < first || end >= pointCount)
                        return false;
                    interpolateUntouched(originalX, tdx, touched, first, end);
                    interpolateUntouched(originalY, tdy, touched, first, end);
                    first = static_cast<std::size_t>(end) + 1U;
                }
            }
            for (std::size_t i = 0; i < total; ++i) {
                dx[i] += scalar * tdx[i];
                dy[i] += scalar * tdy[i];
            }
        }
        return true;
    }

    [[nodiscard]] bool process(const std::uint32_t glyphIndex, const int depth) {
        if (outputs_[glyphIndex])
            return true;
        if (depth > kMaximumComponentDepth)
            return false;
        const auto bytes = glyphBytes(glyphIndex);
        if (!bytes)
            return false;
        auto parsed = parseGlyph(*bytes);
        if (!parsed)
            return false;
        const auto metrics = metricsFor(glyphIndex);
        const auto pointCount = parsed->composite ? parsed->components.size() : parsed->points.size();
        std::vector<double> dx;
        std::vector<double> dy;
        if (!deltasFor(glyphIndex, *parsed, pointCount, dx, dy))
            return false;

        Output out;
        // The advance: from HVAR when the font has it, else from the phantom points.
        double advanceDelta = dx[pointCount + 1] - dx[pointCount];
        if (hvar_)
            if (const auto delta = hvarAdvanceDelta(*hvar_, glyphIndex, location_))
                advanceDelta = *delta;
        out.advance = std::max(0, metrics.advance + static_cast<int>(std::lround(advanceDelta)));

        if (!parsed->composite) {
            if (parsed->points.empty()) {
                outputs_[glyphIndex] = std::move(out);
                return true;
            }
            for (std::size_t i = 0; i < parsed->points.size(); ++i) {
                parsed->points[i].x += static_cast<int>(std::lround(dx[i]));
                parsed->points[i].y += static_cast<int>(std::lround(dy[i]));
            }
            out.xMin = out.xMax = parsed->points.front().x;
            out.yMin = out.yMax = parsed->points.front().y;
            for (const auto& point : parsed->points) {
                out.xMin = std::min(out.xMin, point.x);
                out.xMax = std::max(out.xMax, point.x);
                out.yMin = std::min(out.yMin, point.y);
                out.yMax = std::max(out.yMax, point.y);
            }
            out.empty = false;
            put16(out.data, static_cast<std::uint32_t>(static_cast<std::uint16_t>(parsed->contourEnds.size())));
            putS16(out.data, out.xMin);
            putS16(out.data, out.yMin);
            putS16(out.data, out.xMax);
            putS16(out.data, out.yMax);
            for (const auto end : parsed->contourEnds)
                put16(out.data, end);
            put16(out.data, static_cast<std::uint32_t>(parsed->instructions.size()));
            out.data.insert(out.data.end(), parsed->instructions.begin(),
                            parsed->instructions.end());
            for (std::size_t i = 0; i < parsed->points.size(); ++i)
                out.data.push_back(parsed->onCurve[i] ? 1 : 0);
            int previous = 0;
            for (const auto& point : parsed->points) {
                putS16(out.data, point.x - previous);
                previous = point.x;
            }
            previous = 0;
            for (const auto& point : parsed->points) {
                putS16(out.data, point.y - previous);
                previous = point.y;
            }
        } else {
            bool any = false;
            std::vector<std::uint8_t> body;
            for (std::size_t i = 0; i < parsed->components.size(); ++i) {
                auto& component = parsed->components[i];
                if (component.glyph >= glyphCount_ || !process(component.glyph, depth + 1))
                    return false;
                const bool offsets = (component.flags & kArgsAreXy) != 0;
                if (offsets) {
                    component.dx += static_cast<int>(std::lround(dx[i]));
                    component.dy += static_cast<int>(std::lround(dy[i]));
                }
                const auto& child = *outputs_[component.glyph];
                if (!child.empty) {
                    const std::array<std::pair<int, int>, 4> corners{
                        {{child.xMin, child.yMin}, {child.xMax, child.yMin},
                         {child.xMin, child.yMax}, {child.xMax, child.yMax}}};
                    for (const auto& [cx, cy] : corners) {
                        const auto x = static_cast<int>(std::lround(component.a * cx + component.c * cy)) +
                                       (offsets ? component.dx : 0);
                        const auto y = static_cast<int>(std::lround(component.b * cx + component.d * cy)) +
                                       (offsets ? component.dy : 0);
                        if (!any) {
                            out.xMin = out.xMax = x;
                            out.yMin = out.yMax = y;
                            any = true;
                        }
                        out.xMin = std::min(out.xMin, x);
                        out.xMax = std::max(out.xMax, x);
                        out.yMin = std::min(out.yMin, y);
                        out.yMax = std::max(out.yMax, y);
                    }
                }
                // The instruction flag is on the last component alone, where the program follows.
                auto flags = static_cast<std::uint16_t>(component.flags & ~kHaveInstructions);
                if (i + 1 == parsed->components.size() && !parsed->instructions.empty())
                    flags = static_cast<std::uint16_t>(flags | kHaveInstructions);
                if (offsets)
                    flags = static_cast<std::uint16_t>(flags | kArgsAreWords);
                put16(body, flags);
                put16(body, component.glyph);
                if (offsets) {
                    putS16(body, component.dx);
                    putS16(body, component.dy);
                } else if ((component.flags & kArgsAreWords) != 0) {
                    put16(body, static_cast<std::uint32_t>(component.dx));
                    put16(body, static_cast<std::uint32_t>(component.dy));
                } else {
                    body.push_back(static_cast<std::uint8_t>(component.dx));
                    body.push_back(static_cast<std::uint8_t>(component.dy));
                }
                body.insert(body.end(), component.transform.begin(), component.transform.end());
            }
            out.empty = !any;
            put16(out.data, 0xFFFF);
            putS16(out.data, out.xMin);
            putS16(out.data, out.yMin);
            putS16(out.data, out.xMax);
            putS16(out.data, out.yMax);
            out.data.insert(out.data.end(), body.begin(), body.end());
            if (!parsed->instructions.empty()) {
                put16(out.data, static_cast<std::uint32_t>(parsed->instructions.size()));
                out.data.insert(out.data.end(), parsed->instructions.begin(),
                                parsed->instructions.end());
            }
        }
        while (out.data.size() % 4 != 0)
            out.data.push_back(0);
        outputs_[glyphIndex] = std::move(out);
        return true;
    }

    [[nodiscard]] std::vector<std::uint8_t> rebuild() const {
        std::vector<std::uint8_t> glyf;
        std::vector<std::uint8_t> loca;
        std::vector<std::uint8_t> hmtx;
        for (const auto& output : outputs_) {
            put32(loca, static_cast<std::uint32_t>(glyf.size()));
            glyf.insert(glyf.end(), output->data.begin(), output->data.end());
            put16(hmtx, static_cast<std::uint32_t>(output->advance));
            putS16(hmtx, output->empty ? 0 : output->xMin);
        }
        put32(loca, static_cast<std::uint32_t>(glyf.size()));

        std::map<std::uint32_t, std::vector<std::uint8_t>> tables;
        static constexpr std::array<std::uint32_t, 7> dropped{
            makeTag("fvar"), makeTag("gvar"), makeTag("avar"), makeTag("HVAR"),
            makeTag("VVAR"), makeTag("MVAR"), makeTag("cvar")};
        for (const auto& [tag, span] : sfnt_.tables) {
            if (std::ranges::find(dropped, tag) != dropped.end())
                continue;
            tables[tag].assign(span.begin(), span.end());
        }
        // The control values follow the location, so the hinting programs (`fpgm`, `prep` and the
        // glyphs' own, all carried as they are) fit the instance. A damaged `cvar` leaves the
        // default control values.
        if (const auto cvar = sfnt_.table(makeTag("cvar")); !cvar.empty())
            if (const auto cvt = tables.find(makeTag("cvt ")); cvt != tables.end())
                if (auto varied = variedControlValues(sfnt_.table(makeTag("cvt ")), cvar,
                                                      location_.size(), location_))
                    cvt->second = std::move(*varied);
        tables[makeTag("glyf")] = std::move(glyf);
        tables[makeTag("loca")] = std::move(loca);
        tables[makeTag("hmtx")] = std::move(hmtx);
        auto& head = tables[makeTag("head")];
        head[50] = 0;
        head[51] = 1; // Long loca offsets.
        auto& hhea = tables[makeTag("hhea")];
        hhea[34] = static_cast<std::uint8_t>((glyphCount_ >> 8U) & 0xFFU);
        hhea[35] = static_cast<std::uint8_t>(glyphCount_ & 0xFFU);

        return writeSfnt(0x00010000, std::move(tables));
    }

    const Sfnt& sfnt_;
    std::vector<double> location_;
    std::span<const std::uint8_t> hmtx_;
    std::span<const std::uint8_t> loca_;
    std::span<const std::uint8_t> glyf_;
    bool longLoca_ = false;
    std::uint32_t glyphCount_ = 0;
    std::uint16_t metricCount_ = 0;
    Gvar gvar_;
    std::optional<Hvar> hvar_;
    std::vector<std::optional<Output>> outputs_;
};

} // namespace

std::vector<VariableFontAxis> variableFontAxes(const std::span<const std::uint8_t> font,
                                               const std::uint32_t collectionIndex) {
    std::vector<VariableFontAxis> axes;
    const auto sfnt = parseSfnt(font, collectionIndex);
    if (!sfnt)
        return axes;
    const auto fvar = parseFvar(sfnt->table(makeTag("fvar")));
    if (!fvar)
        return axes;
    for (const auto& axis : fvar->axes)
        axes.push_back({tagString(axis.tag), axis.minimum, axis.defaultValue, axis.maximum});
    return axes;
}

VariableFontInstance instantiateVariableFont(const std::span<const std::uint8_t> font,
                                             const std::uint32_t collectionIndex,
                                             const std::uint32_t namedInstance,
                                             const std::span<const VariableFontAxisSetting> settings) {
    VariableFontInstance result;
    const auto sfnt = parseSfnt(font, collectionIndex);
    if (!sfnt) {
        result.status = VariableFontInstanceStatus::Unsupported;
        result.detail = "an unreadable file, or a collection without that font";
        return result;
    }
    if (sfnt->table(makeTag("fvar")).empty())
        return result;
    result.status = VariableFontInstanceStatus::Unsupported;
    const auto fvar = parseFvar(sfnt->table(makeTag("fvar")));
    if (!fvar) {
        result.detail = "damaged variation axes";
        return result;
    }
    const bool compact = !sfnt->table(makeTag("CFF2")).empty();
    if (!compact &&
        (sfnt->table(makeTag("gvar")).empty() || sfnt->table(makeTag("glyf")).empty())) {
        result.detail = "outlines are neither TrueType glyph data with variations nor CFF2";
        return result;
    }
    std::vector<double> user;
    for (const auto& axis : fvar->axes)
        user.push_back(axis.defaultValue);
    if (namedInstance >= 1 && namedInstance <= fvar->instances.size())
        user = fvar->instances[namedInstance - 1U];
    for (const auto& setting : settings)
        for (std::size_t i = 0; i < fvar->axes.size(); ++i)
            if (tagString(fvar->axes[i].tag) == setting.tag)
                user[i] = setting.value;
    const auto avar = parseAvar(sfnt->table(makeTag("avar")), fvar->axes.size());
    auto location = normalize(*fvar, user, avar);
    std::optional<std::vector<std::uint8_t>> bytes;
    if (compact) {
        bytes = instantiateCff2(*sfnt, location);
    } else {
        Instancer instancer(*sfnt, std::move(location));
        bytes = instancer.run(*fvar);
    }
    if (!bytes) {
        result.detail = compact ? "damaged CFF2 outlines or variation data"
                                : "damaged variation tables";
        return result;
    }
    result.status = VariableFontInstanceStatus::Instanced;
    result.bytes = std::move(*bytes);
    result.detail.clear();
    return result;
}

} // namespace bloom::runtime::fonts
