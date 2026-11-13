// #489: a variable font whose outlines are compact font format data (OpenType `CFF2`) at one point
// of its design space, written back as a static font with a `CFF ` table.
//
// A CFF2 charstring holds the default outline and, at each `blend` operator, the deltas of the
// values before it for every region of the font's variation store. This file runs every glyph's
// charstring at the location (subroutines, hints and flex operators included), keeps the outline
// that results and writes it as a Type 2 charstring of moves, lines and curves in whole font
// units. The static table is CID-keyed with one font dictionary, the form every reader of CFF
// outlines takes; hint operators are not carried, since the stems they name move with the
// outline and the rasterizer here reads none. Advances follow `HVAR` when the font has one.
// The formats are the public OpenType tables CFF2, CFF and the item variation store.

#include "variable_font_tables.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::runtime::fonts::detail {
namespace {

constexpr std::size_t kMaximumStack = 513;
constexpr int kMaximumSubroutineDepth = 10;
// The most path segments one glyph may hold; a damaged charstring ends here, not in memory.
constexpr std::size_t kMaximumSegments = 100'000;

// An INDEX: `count` objects whose bytes are `object(i)`.
struct Index final {
    std::span<const std::uint8_t> table;
    std::uint32_t count = 0;
    std::uint8_t offsetSize = 0;
    std::size_t offsetsAt = 0;
    std::size_t dataAt = 0;
    // The position after the INDEX.
    std::size_t end = 0;

    [[nodiscard]] std::optional<std::span<const std::uint8_t>> object(const std::uint32_t index) const {
        if (index >= count)
            return std::nullopt;
        Reader reader(table, offsetsAt + static_cast<std::size_t>(index) * offsetSize);
        std::size_t begin = 0;
        std::size_t finish = 0;
        for (std::uint8_t i = 0; i < offsetSize; ++i)
            begin = (begin << 8U) | reader.u8();
        for (std::uint8_t i = 0; i < offsetSize; ++i)
            finish = (finish << 8U) | reader.u8();
        if (reader.failed() || begin < 1 || finish < begin || dataAt + finish - 1 > table.size())
            return std::nullopt;
        return table.subspan(dataAt + begin - 1, finish - begin);
    }
};

// A CFF2 INDEX at `offset` (a 32-bit count; an empty INDEX is the count alone).
[[nodiscard]] std::optional<Index> parseIndex(const std::span<const std::uint8_t> table,
                                              const std::size_t offset) {
    Reader reader(table, offset);
    Index index;
    index.table = table;
    index.count = reader.u32();
    if (reader.failed())
        return std::nullopt;
    if (index.count == 0) {
        index.end = offset + 4;
        return index;
    }
    index.offsetSize = reader.u8();
    if (reader.failed() || index.offsetSize < 1 || index.offsetSize > 4)
        return std::nullopt;
    index.offsetsAt = reader.position();
    const auto offsetsBytes = (static_cast<std::size_t>(index.count) + 1U) * index.offsetSize;
    if (offsetsBytes > table.size() || index.offsetsAt > table.size() - offsetsBytes)
        return std::nullopt;
    index.dataAt = index.offsetsAt + offsetsBytes;
    Reader last(table, index.offsetsAt + static_cast<std::size_t>(index.count) * index.offsetSize);
    std::size_t finish = 0;
    for (std::uint8_t i = 0; i < index.offsetSize; ++i)
        finish = (finish << 8U) | last.u8();
    if (last.failed() || finish < 1 || index.dataAt + finish - 1 > table.size())
        return std::nullopt;
    index.end = index.dataAt + finish - 1;
    return index;
}

// One operator of a DICT with the operands before it. Reals are read as numbers; `raw` is the
// operands' own bytes, for a value that is carried over as it is written.
struct DictEntry final {
    std::uint16_t op = 0; // Two-byte operators are 0x0C00 | second byte.
    std::vector<double> operands;
    std::span<const std::uint8_t> raw;
};

[[nodiscard]] std::optional<std::vector<DictEntry>> parseDict(const std::span<const std::uint8_t> dict) {
    std::vector<DictEntry> entries;
    DictEntry current;
    std::size_t operandsAt = 0;
    std::size_t at = 0;
    while (at < dict.size()) {
        const auto b0 = dict[at];
        if (b0 <= 27) {
            current.raw = dict.subspan(operandsAt, at - operandsAt);
            ++at;
            current.op = b0;
            if (b0 == 12) {
                if (at >= dict.size())
                    return std::nullopt;
                current.op = static_cast<std::uint16_t>(0x0C00U | dict[at]);
                ++at;
            }
            entries.push_back(std::move(current));
            current = {};
            operandsAt = at;
            continue;
        }
        double value = 0.0;
        if (b0 == 28) {
            if (at + 3 > dict.size())
                return std::nullopt;
            value = static_cast<std::int16_t>(static_cast<std::uint16_t>((dict[at + 1] << 8U) | dict[at + 2]));
            at += 3;
        } else if (b0 == 29) {
            if (at + 5 > dict.size())
                return std::nullopt;
            const auto raw = (static_cast<std::uint32_t>(dict[at + 1]) << 24U) |
                             (static_cast<std::uint32_t>(dict[at + 2]) << 16U) |
                             (static_cast<std::uint32_t>(dict[at + 3]) << 8U) |
                             static_cast<std::uint32_t>(dict[at + 4]);
            value = static_cast<std::int32_t>(raw);
            at += 5;
        } else if (b0 == 30) {
            // A real number in binary-coded decimal nibbles, ended by nibble 0xF.
            ++at;
            double mantissa = 0.0;
            double scale = 1.0;
            int exponent = 0;
            int exponentSign = 1;
            bool negative = false;
            bool fraction = false;
            bool inExponent = false;
            bool ended = false;
            while (!ended) {
                if (at >= dict.size())
                    return std::nullopt;
                const auto byte = dict[at++];
                for (const auto nibble : {static_cast<std::uint8_t>(byte >> 4U),
                                          static_cast<std::uint8_t>(byte & 0x0FU)}) {
                    if (ended)
                        break;
                    if (nibble <= 9) {
                        if (inExponent) {
                            exponent = std::min(exponent * 10 + nibble, 400);
                        } else if (fraction) {
                            scale /= 10.0;
                            mantissa += nibble * scale;
                        } else {
                            mantissa = mantissa * 10.0 + nibble;
                        }
                    } else if (nibble == 0xA) {
                        fraction = true;
                    } else if (nibble == 0xB) {
                        inExponent = true;
                    } else if (nibble == 0xC) {
                        inExponent = true;
                        exponentSign = -1;
                    } else if (nibble == 0xE) {
                        negative = true;
                    } else if (nibble == 0xF) {
                        ended = true;
                    } else {
                        return std::nullopt;
                    }
                }
            }
            value = mantissa * std::pow(10.0, exponentSign * exponent);
            if (negative)
                value = -value;
        } else if (b0 >= 32 && b0 <= 246) {
            value = static_cast<int>(b0) - 139;
            ++at;
        } else if (b0 >= 247 && b0 <= 250) {
            if (at + 2 > dict.size())
                return std::nullopt;
            value = (static_cast<int>(b0) - 247) * 256 + dict[at + 1] + 108;
            at += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            if (at + 2 > dict.size())
                return std::nullopt;
            value = -(static_cast<int>(b0) - 251) * 256 - dict[at + 1] - 108;
            at += 2;
        } else {
            return std::nullopt;
        }
        current.operands.push_back(value);
    }
    return entries;
}

[[nodiscard]] const DictEntry* find(const std::vector<DictEntry>& dict, const std::uint16_t op) {
    for (const auto& entry : dict)
        if (entry.op == op)
            return &entry;
    return nullptr;
}

// A DICT operand that is an offset or a size: a whole number that fits the table.
[[nodiscard]] std::optional<std::size_t> offsetOperand(const DictEntry* entry, const std::size_t which,
                                                       const std::size_t limit) {
    if (entry == nullptr || which >= entry->operands.size())
        return std::nullopt;
    const auto value = entry->operands[which];
    if (!(value >= 0.0) || value > static_cast<double>(limit))
        return std::nullopt;
    return static_cast<std::size_t>(value);
}

constexpr std::uint16_t kOpCharStrings = 17;
constexpr std::uint16_t kOpPrivate = 18;
constexpr std::uint16_t kOpSubrs = 19;
constexpr std::uint16_t kOpVsindex = 22;
constexpr std::uint16_t kOpVstore = 24;
constexpr std::uint16_t kOpFontMatrix = 0x0C07;
constexpr std::uint16_t kOpFdArray = 0x0C24;
constexpr std::uint16_t kOpFdSelect = 0x0C25;

// What one font dictionary gives its glyphs: its local subroutines and the variation data its
// charstrings start with.
struct FontDict final {
    Index subrs;
    std::uint32_t vsindex = 0;
};

// The scalar of every region of each item variation data at the location: what a `blend`
// multiplies its deltas by.
[[nodiscard]] std::optional<std::vector<std::vector<double>>>
regionScalars(const std::span<const std::uint8_t> table, const std::size_t storeAt,
              const std::vector<double>& location) {
    Reader store(table, storeAt);
    const auto format = store.u16();
    const auto regionListOffset = store.u32();
    const auto dataCount = store.u16();
    if (store.failed() || format != 1)
        return std::nullopt;
    Reader regions(table, storeAt + regionListOffset);
    const auto axisCount = regions.u16();
    const auto regionCount = regions.u16();
    if (regions.failed() || axisCount != location.size())
        return std::nullopt;
    std::vector<double> perRegion(regionCount, 0.0);
    for (std::uint16_t r = 0; r < regionCount; ++r) {
        std::vector<double> start(axisCount);
        std::vector<double> peak(axisCount);
        std::vector<double> end(axisCount);
        for (std::uint16_t a = 0; a < axisCount; ++a) {
            start[a] = regions.f2dot14();
            peak[a] = regions.f2dot14();
            end[a] = regions.f2dot14();
        }
        if (regions.failed())
            return std::nullopt;
        perRegion[r] = regionScalar(location, start, peak, end);
    }
    std::vector<std::vector<double>> scalars;
    for (std::uint16_t d = 0; d < dataCount; ++d) {
        Reader offsets(table, storeAt + 8 + static_cast<std::size_t>(d) * 4U);
        const auto dataOffset = offsets.u32();
        Reader data(table, storeAt + dataOffset);
        data.skip(4); // itemCount and wordDeltaCount: a CFF2 store holds the regions only.
        const auto indexCount = data.u16();
        if (offsets.failed() || data.failed())
            return std::nullopt;
        std::vector<double> row;
        row.reserve(indexCount);
        for (std::uint16_t i = 0; i < indexCount; ++i) {
            const auto region = data.u16();
            if (data.failed() || region >= regionCount)
                return std::nullopt;
            row.push_back(perRegion[region]);
        }
        scalars.push_back(std::move(row));
    }
    return scalars;
}

[[nodiscard]] int subroutineBias(const std::uint32_t count) noexcept {
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

struct Segment final {
    enum class Kind : std::uint8_t { Move, Line, Curve };
    Kind kind = Kind::Move;
    // Absolute font units: the end point, and for a curve its two control points before it.
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double x = 0.0;
    double y = 0.0;
};

// Runs one CFF2 charstring at a location and collects its outline.
class Charstring final {
  public:
    Charstring(const Index& globalSubrs, const FontDict& dict,
               const std::vector<std::vector<double>>& scalars)
        : globalSubrs_(globalSubrs), dict_(dict), scalars_(scalars), vsindex_(dict.vsindex) {}

    [[nodiscard]] bool run(const std::span<const std::uint8_t> code) { return execute(code, 0); }
    [[nodiscard]] std::vector<Segment>& segments() noexcept { return segments_; }

  private:
    [[nodiscard]] bool push(const double value) {
        if (stack_.size() >= kMaximumStack)
            return false;
        stack_.push_back(value);
        return true;
    }
    [[nodiscard]] bool add(const Segment::Kind kind, const double x1, const double y1,
                           const double x2, const double y2, const double x, const double y) {
        if (segments_.size() >= kMaximumSegments)
            return false;
        segments_.push_back({kind, x1, y1, x2, y2, x, y});
        x_ = x;
        y_ = y;
        return true;
    }
    [[nodiscard]] bool moveBy(const double dx, const double dy) {
        return add(Segment::Kind::Move, 0, 0, 0, 0, x_ + dx, y_ + dy);
    }
    [[nodiscard]] bool lineBy(const double dx, const double dy) {
        return add(Segment::Kind::Line, 0, 0, 0, 0, x_ + dx, y_ + dy);
    }
    [[nodiscard]] bool curveBy(const double dx1, const double dy1, const double dx2, const double dy2,
                               const double dx3, const double dy3) {
        const double x1 = x_ + dx1;
        const double y1 = y_ + dy1;
        const double x2 = x1 + dx2;
        const double y2 = y1 + dy2;
        return add(Segment::Kind::Curve, x1, y1, x2, y2, x2 + dx3, y2 + dy3);
    }

    [[nodiscard]] bool blend() {
        if (stack_.empty() || vsindex_ >= scalars_.size())
            return false;
        const auto& scalars = scalars_[vsindex_];
        const double countValue = stack_.back();
        stack_.pop_back();
        if (!(countValue >= 0.0) || countValue > static_cast<double>(kMaximumStack))
            return false;
        const auto count = static_cast<std::size_t>(countValue);
        const auto needed = count * (scalars.size() + 1U);
        if (needed > stack_.size())
            return false;
        const auto base = stack_.size() - needed;
        for (std::size_t i = 0; i < count; ++i) {
            double value = stack_[base + i];
            for (std::size_t r = 0; r < scalars.size(); ++r)
                value += stack_[base + count + i * scalars.size() + r] * scalars[r];
            stack_[base + i] = value;
        }
        stack_.resize(base + count);
        return true;
    }

    [[nodiscard]] bool call(const Index& subrs, const int depth) {
        if (stack_.empty() || depth >= kMaximumSubroutineDepth)
            return false;
        const double number = stack_.back() + subroutineBias(subrs.count);
        stack_.pop_back();
        if (!(number >= 0.0) || number >= static_cast<double>(subrs.count))
            return false;
        const auto code = subrs.object(static_cast<std::uint32_t>(number));
        return code && execute(*code, depth + 1);
    }

    [[nodiscard]] bool execute(const std::span<const std::uint8_t> code, const int depth) {
        std::size_t at = 0;
        const auto arguments = [this] { return stack_.size(); };
        while (at < code.size()) {
            const auto b0 = code[at++];
            if (b0 == 28) {
                if (at + 2 > code.size())
                    return false;
                const auto raw = static_cast<std::uint16_t>((code[at] << 8U) | code[at + 1]);
                at += 2;
                if (!push(static_cast<std::int16_t>(raw)))
                    return false;
                continue;
            }
            if (b0 >= 32 && b0 <= 246) {
                if (!push(static_cast<int>(b0) - 139))
                    return false;
                continue;
            }
            if (b0 >= 247 && b0 <= 254) {
                if (at >= code.size())
                    return false;
                const int low = code[at++];
                const int value = b0 <= 250 ? (static_cast<int>(b0) - 247) * 256 + low + 108
                                            : -(static_cast<int>(b0) - 251) * 256 - low - 108;
                if (!push(value))
                    return false;
                continue;
            }
            if (b0 == 255) {
                if (at + 4 > code.size())
                    return false;
                const auto raw = (static_cast<std::uint32_t>(code[at]) << 24U) |
                                 (static_cast<std::uint32_t>(code[at + 1]) << 16U) |
                                 (static_cast<std::uint32_t>(code[at + 2]) << 8U) |
                                 static_cast<std::uint32_t>(code[at + 3]);
                at += 4;
                if (!push(static_cast<double>(static_cast<std::int32_t>(raw)) / 65536.0))
                    return false;
                continue;
            }
            bool ok = true;
            switch (b0) {
            case 1:  // hstem
            case 3:  // vstem
            case 18: // hstemhm
            case 23: // vstemhm
                stems_ += arguments() / 2;
                break;
            case 19: // hintmask
            case 20: // cntrmask
                stems_ += arguments() / 2; // Stems left on the stack are vertical stems.
                at += (stems_ + 7) / 8;
                if (at > code.size())
                    return false;
                break;
            case 21: // rmoveto
                ok = arguments() >= 2 &&
                     moveBy(stack_[arguments() - 2], stack_[arguments() - 1]);
                break;
            case 22: // hmoveto
                ok = arguments() >= 1 && moveBy(stack_.back(), 0.0);
                break;
            case 4: // vmoveto
                ok = arguments() >= 1 && moveBy(0.0, stack_.back());
                break;
            case 5: // rlineto
                for (std::size_t i = 0; ok && i + 1 < arguments(); i += 2)
                    ok = lineBy(stack_[i], stack_[i + 1]);
                break;
            case 6:   // hlineto
            case 7: { // vlineto
                bool horizontal = b0 == 6;
                for (std::size_t i = 0; ok && i < arguments(); ++i) {
                    ok = horizontal ? lineBy(stack_[i], 0.0) : lineBy(0.0, stack_[i]);
                    horizontal = !horizontal;
                }
                break;
            }
            case 8: // rrcurveto
                for (std::size_t i = 0; ok && i + 5 < arguments(); i += 6)
                    ok = curveBy(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4],
                                 stack_[i + 5]);
                break;
            case 24: { // rcurveline
                std::size_t i = 0;
                for (; ok && i + 7 < arguments(); i += 6)
                    ok = curveBy(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4],
                                 stack_[i + 5]);
                ok = ok && i + 1 < arguments() && lineBy(stack_[i], stack_[i + 1]);
                break;
            }
            case 25: { // rlinecurve
                std::size_t i = 0;
                for (; ok && i + 7 < arguments(); i += 2)
                    ok = lineBy(stack_[i], stack_[i + 1]);
                ok = ok && i + 5 < arguments() &&
                     curveBy(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4],
                             stack_[i + 5]);
                break;
            }
            case 26: { // vvcurveto
                std::size_t i = 0;
                double dx1 = 0.0;
                if (arguments() % 4 == 1)
                    dx1 = stack_[i++];
                for (; ok && i + 3 < arguments(); i += 4) {
                    ok = curveBy(dx1, stack_[i], stack_[i + 1], stack_[i + 2], 0.0, stack_[i + 3]);
                    dx1 = 0.0;
                }
                break;
            }
            case 27: { // hhcurveto
                std::size_t i = 0;
                double dy1 = 0.0;
                if (arguments() % 4 == 1)
                    dy1 = stack_[i++];
                for (; ok && i + 3 < arguments(); i += 4) {
                    ok = curveBy(stack_[i], dy1, stack_[i + 1], stack_[i + 2], stack_[i + 3], 0.0);
                    dy1 = 0.0;
                }
                break;
            }
            case 30:   // vhcurveto
            case 31: { // hvcurveto
                bool horizontal = b0 == 31;
                std::size_t i = 0;
                while (ok && i + 3 < arguments()) {
                    const bool last = arguments() - i == 5;
                    const double extra = last ? stack_[i + 4] : 0.0;
                    if (horizontal)
                        ok = curveBy(stack_[i], 0.0, stack_[i + 1], stack_[i + 2], extra, stack_[i + 3]);
                    else
                        ok = curveBy(0.0, stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], extra);
                    horizontal = !horizontal;
                    i += last ? 5 : 4;
                }
                break;
            }
            case 10: // callsubr
                if (!call(dict_.subrs, depth))
                    return false;
                continue;
            case 29: // callgsubr
                if (!call(globalSubrs_, depth))
                    return false;
                continue;
            case 15: { // vsindex
                ok = arguments() >= 1 && stack_.back() >= 0.0 &&
                     stack_.back() < static_cast<double>(scalars_.size());
                if (ok)
                    vsindex_ = static_cast<std::size_t>(stack_.back());
                break;
            }
            case 16: // blend
                if (!blend())
                    return false;
                continue;
            case 12: {
                if (at >= code.size())
                    return false;
                const auto b1 = code[at++];
                const auto& s = stack_;
                if (b1 == 34 && arguments() >= 7) { // hflex
                    ok = curveBy(s[0], 0.0, s[1], s[2], s[3], 0.0) &&
                         curveBy(s[4], 0.0, s[5], -s[2], s[6], 0.0);
                } else if (b1 == 35 && arguments() >= 12) { // flex
                    ok = curveBy(s[0], s[1], s[2], s[3], s[4], s[5]) &&
                         curveBy(s[6], s[7], s[8], s[9], s[10], s[11]);
                } else if (b1 == 36 && arguments() >= 9) { // hflex1
                    ok = curveBy(s[0], s[1], s[2], s[3], s[4], 0.0) &&
                         curveBy(s[5], 0.0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
                } else if (b1 == 37 && arguments() >= 11) { // flex1
                    const double dx = s[0] + s[2] + s[4] + s[6] + s[8];
                    const double dy = s[1] + s[3] + s[5] + s[7] + s[9];
                    const bool wide = std::fabs(dx) > std::fabs(dy);
                    ok = curveBy(s[0], s[1], s[2], s[3], s[4], s[5]) &&
                         curveBy(s[6], s[7], s[8], s[9], wide ? s[10] : -dx, wide ? -dy : s[10]);
                } else {
                    return false;
                }
                break;
            }
            default:
                // Operators a CFF2 charstring does not have (endchar, return and the Type 2
                // arithmetic set among them).
                return false;
            }
            if (!ok)
                return false;
            stack_.clear();
        }
        return true;
    }

    const Index& globalSubrs_;
    const FontDict& dict_;
    const std::vector<std::vector<double>>& scalars_;
    std::size_t vsindex_ = 0;
    std::vector<double> stack_;
    std::vector<Segment> segments_;
    std::size_t stems_ = 0;
    double x_ = 0.0;
    double y_ = 0.0;
};

// A whole number as a Type 2 charstring operand. False when it does not fit 16 bits.
[[nodiscard]] bool putNumber(std::vector<std::uint8_t>& out, const long value) {
    if (value >= -107 && value <= 107) {
        out.push_back(static_cast<std::uint8_t>(value + 139));
    } else if (value >= 108 && value <= 1131) {
        const auto v = value - 108;
        out.push_back(static_cast<std::uint8_t>(247 + v / 256));
        out.push_back(static_cast<std::uint8_t>(v % 256));
    } else if (value >= -1131 && value <= -108) {
        const auto v = -value - 108;
        out.push_back(static_cast<std::uint8_t>(251 + v / 256));
        out.push_back(static_cast<std::uint8_t>(v % 256));
    } else if (value >= -32768 && value <= 32767) {
        out.push_back(28);
        out.push_back(static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) >> 8U) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) & 0xFFU));
    } else {
        return false;
    }
    return true;
}

// A DICT operand of a fixed five bytes, so an offset can be written before it is known to be small.
void putDictInteger(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(29);
    put32(out, value);
}

void putOperator(std::vector<std::uint8_t>& out, const std::uint16_t op) {
    if (op >= 0x0C00U)
        out.push_back(12);
    out.push_back(static_cast<std::uint8_t>(op & 0xFFU));
}

// A CFF (version 1) INDEX of `objects`, with four-byte offsets.
void putIndex(std::vector<std::uint8_t>& out, const std::vector<std::vector<std::uint8_t>>& objects) {
    put16(out, static_cast<std::uint32_t>(objects.size()));
    if (objects.empty())
        return;
    out.push_back(4);
    std::uint32_t offset = 1;
    put32(out, offset);
    for (const auto& object : objects) {
        offset += static_cast<std::uint32_t>(object.size());
        put32(out, offset);
    }
    for (const auto& object : objects)
        out.insert(out.end(), object.begin(), object.end());
}

[[nodiscard]] std::size_t indexSize(const std::vector<std::vector<std::uint8_t>>& objects) {
    if (objects.empty())
        return 2;
    std::size_t bytes = 3 + (objects.size() + 1U) * 4U;
    for (const auto& object : objects)
        bytes += object.size();
    return bytes;
}

struct GlyphOutline final {
    std::vector<std::uint8_t> charstring;
    bool empty = true;
    long xMin = 0;
    long yMin = 0;
    long xMax = 0;
    long yMax = 0;
};

// The outline as a Type 2 charstring: the advance, then one operator a segment, in whole units.
[[nodiscard]] std::optional<GlyphOutline> encodeGlyph(const std::vector<Segment>& segments,
                                                      const long advance) {
    GlyphOutline glyph;
    auto& out = glyph.charstring;
    if (!putNumber(out, advance))
        return std::nullopt;
    long x = 0;
    long y = 0;
    const auto whole = [](const double value) -> std::optional<long> {
        if (!std::isfinite(value) || std::fabs(value) > 32767.0)
            return std::nullopt;
        return std::lround(value);
    };
    const auto bound = [&glyph](const long px, const long py) {
        if (glyph.empty) {
            glyph.xMin = glyph.xMax = px;
            glyph.yMin = glyph.yMax = py;
            glyph.empty = false;
            return;
        }
        glyph.xMin = std::min(glyph.xMin, px);
        glyph.xMax = std::max(glyph.xMax, px);
        glyph.yMin = std::min(glyph.yMin, py);
        glyph.yMax = std::max(glyph.yMax, py);
    };
    // An outline starts with a move; the advance before the first operator relies on it.
    if (!segments.empty() && segments.front().kind != Segment::Kind::Move)
        return std::nullopt;
    // A move that no line or curve follows draws nothing and is left out.
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        const auto ex = whole(segment.x);
        const auto ey = whole(segment.y);
        if (!ex || !ey)
            return std::nullopt;
        if (segment.kind == Segment::Kind::Move) {
            if (i + 1 >= segments.size() || segments[i + 1].kind == Segment::Kind::Move)
                continue;
            if (!putNumber(out, *ex - x) || !putNumber(out, *ey - y))
                return std::nullopt;
            out.push_back(21);
            bound(*ex, *ey);
        } else if (segment.kind == Segment::Kind::Line) {
            if (!putNumber(out, *ex - x) || !putNumber(out, *ey - y))
                return std::nullopt;
            out.push_back(5);
            bound(*ex, *ey);
        } else {
            const auto ax = whole(segment.x1);
            const auto ay = whole(segment.y1);
            const auto bx = whole(segment.x2);
            const auto by = whole(segment.y2);
            if (!ax || !ay || !bx || !by || !putNumber(out, *ax - x) || !putNumber(out, *ay - y) ||
                !putNumber(out, *bx - *ax) || !putNumber(out, *by - *ay) ||
                !putNumber(out, *ex - *bx) || !putNumber(out, *ey - *by))
                return std::nullopt;
            out.push_back(8);
            bound(*ax, *ay);
            bound(*bx, *by);
            bound(*ex, *ey);
        }
        x = *ex;
        y = *ey;
    }
    out.push_back(14); // endchar
    return glyph;
}

} // namespace

std::optional<std::vector<std::uint8_t>> instantiateCff2(const Sfnt& sfnt,
                                                         const std::vector<double>& location) {
    const auto table = sfnt.table(makeTag("CFF2"));
    const auto head = sfnt.table(makeTag("head"));
    const auto maxp = sfnt.table(makeTag("maxp"));
    const auto hhea = sfnt.table(makeTag("hhea"));
    const auto hmtx = sfnt.table(makeTag("hmtx"));
    if (table.size() < 5 || head.size() < 54 || maxp.size() < 6 || hhea.size() < 36 || hmtx.empty())
        return std::nullopt;
    Reader header(table);
    const auto major = header.u8();
    header.skip(1);
    const auto headerSize = header.u8();
    const auto topDictLength = header.u16();
    if (header.failed() || major != 2 || headerSize < 5 ||
        static_cast<std::size_t>(headerSize) + topDictLength > table.size())
        return std::nullopt;
    const auto top = parseDict(table.subspan(headerSize, topDictLength));
    if (!top)
        return std::nullopt;
    const auto globalSubrs = parseIndex(table, static_cast<std::size_t>(headerSize) + topDictLength);
    const auto charStringsAt = offsetOperand(find(*top, kOpCharStrings), 0, table.size());
    const auto fdArrayAt = offsetOperand(find(*top, kOpFdArray), 0, table.size());
    if (!globalSubrs || !charStringsAt || !fdArrayAt)
        return std::nullopt;
    const auto charStrings = parseIndex(table, *charStringsAt);
    const auto fdArray = parseIndex(table, *fdArrayAt);
    Reader maxpReader(maxp, 4);
    const std::uint32_t glyphCount = maxpReader.u16();
    Reader hheaReader(hhea, 34);
    const std::uint32_t metricCount = hheaReader.u16();
    if (!charStrings || !fdArray || fdArray->count == 0 || glyphCount == 0 ||
        charStrings->count != glyphCount || metricCount == 0 || metricCount > glyphCount ||
        hmtx.size() < static_cast<std::size_t>(metricCount) * 4U +
                          static_cast<std::size_t>(glyphCount - metricCount) * 2U)
        return std::nullopt;

    // The variation store is behind a two-byte length. A font without one has no blends.
    std::vector<std::vector<double>> scalars;
    if (const auto* vstore = find(*top, kOpVstore)) {
        const auto at = offsetOperand(vstore, 0, table.size());
        if (!at)
            return std::nullopt;
        auto parsed = regionScalars(table, *at + 2, location);
        if (!parsed)
            return std::nullopt;
        scalars = std::move(*parsed);
    }

    std::vector<FontDict> dicts;
    for (std::uint32_t i = 0; i < fdArray->count; ++i) {
        const auto bytes = fdArray->object(i);
        const auto dict = bytes ? parseDict(*bytes) : std::nullopt;
        if (!dict)
            return std::nullopt;
        FontDict font;
        font.subrs.table = table;
        if (const auto* priv = find(*dict, kOpPrivate)) {
            const auto size = offsetOperand(priv, 0, table.size());
            const auto at = offsetOperand(priv, 1, table.size());
            if (!size || !at || *size > table.size() - *at)
                return std::nullopt;
            const auto privateDict = parseDict(table.subspan(*at, *size));
            if (!privateDict)
                return std::nullopt;
            if (const auto* vsindex = find(*privateDict, kOpVsindex);
                vsindex != nullptr && !vsindex->operands.empty() && vsindex->operands.back() >= 0.0)
                font.vsindex = static_cast<std::uint32_t>(vsindex->operands.back());
            if (const auto* subrs = find(*privateDict, kOpSubrs)) {
                const auto relative = offsetOperand(subrs, 0, table.size());
                if (!relative || *relative > table.size() - *at)
                    return std::nullopt;
                const auto index = parseIndex(table, *at + *relative);
                if (!index)
                    return std::nullopt;
                font.subrs = *index;
            }
        }
        dicts.push_back(font);
    }

    // Which font dictionary a glyph uses. One dictionary needs no table.
    std::vector<std::uint16_t> glyphDict(glyphCount, 0);
    if (const auto* select = find(*top, kOpFdSelect)) {
        const auto at = offsetOperand(select, 0, table.size());
        if (!at)
            return std::nullopt;
        Reader reader(table, *at);
        const auto format = reader.u8();
        if (format == 0) {
            for (auto& entry : glyphDict)
                entry = reader.u8();
        } else if (format == 3 || format == 4) {
            const std::uint32_t ranges = format == 3 ? reader.u16() : reader.u32();
            std::uint32_t first = format == 3 ? reader.u16() : reader.u32();
            for (std::uint32_t r = 0; r < ranges && !reader.failed(); ++r) {
                const std::uint16_t fd = format == 3 ? reader.u8() : reader.u16();
                const std::uint32_t next = format == 3 ? reader.u16() : reader.u32();
                if (next < first || next > glyphCount)
                    return std::nullopt;
                for (std::uint32_t g = first; g < next; ++g)
                    glyphDict[g] = fd;
                first = next;
            }
        } else {
            return std::nullopt;
        }
        if (reader.failed())
            return std::nullopt;
    }

    std::optional<Hvar> hvar;
    if (const auto hvarTable = sfnt.table(makeTag("HVAR")); !hvarTable.empty()) {
        Reader hv(hvarTable, 4);
        Hvar parsed;
        parsed.table = hvarTable;
        parsed.storeOffset = hv.u32();
        parsed.advanceMapOffset = hv.u32();
        if (!hv.failed())
            hvar = parsed;
    }

    std::vector<std::vector<std::uint8_t>> charstrings;
    charstrings.reserve(glyphCount);
    std::vector<std::uint8_t> newHmtx;
    bool anyOutline = false;
    long xMin = 0;
    long yMin = 0;
    long xMax = 0;
    long yMax = 0;
    for (std::uint32_t g = 0; g < glyphCount; ++g) {
        const auto code = charStrings->object(g);
        if (!code || glyphDict[g] >= dicts.size())
            return std::nullopt;
        Charstring charstring(*globalSubrs, dicts[glyphDict[g]], scalars);
        if (!charstring.run(*code))
            return std::nullopt;
        Reader advanceReader(hmtx, static_cast<std::size_t>(std::min(g, metricCount - 1U)) * 4U);
        double advance = advanceReader.u16();
        if (hvar) {
            const auto delta = hvarAdvanceDelta(*hvar, g, location);
            if (!delta)
                return std::nullopt;
            advance += *delta;
        }
        const auto wholeAdvance = std::clamp(std::lround(advance), 0L, 65535L);
        auto glyph = encodeGlyph(charstring.segments(), std::min(wholeAdvance, 32767L));
        if (!glyph)
            return std::nullopt;
        put16(newHmtx, static_cast<std::uint32_t>(wholeAdvance));
        putS16(newHmtx, glyph->empty ? 0 : static_cast<int>(glyph->xMin));
        if (!glyph->empty) {
            xMin = anyOutline ? std::min(xMin, glyph->xMin) : glyph->xMin;
            yMin = anyOutline ? std::min(yMin, glyph->yMin) : glyph->yMin;
            xMax = anyOutline ? std::max(xMax, glyph->xMax) : glyph->xMax;
            yMax = anyOutline ? std::max(yMax, glyph->yMax) : glyph->yMax;
            anyOutline = true;
        }
        charstrings.push_back(std::move(glyph->charstring));
    }

    // ---- the static `CFF ` table: CID-keyed, one font dictionary ----
    const std::string_view fontName = "BloomInstance";
    const std::vector<std::vector<std::uint8_t>> names{{fontName.begin(), fontName.end()}};
    const std::string_view registry = "Adobe";
    const std::string_view ordering = "Identity";
    const std::vector<std::vector<std::uint8_t>> strings{{registry.begin(), registry.end()},
                                                         {ordering.begin(), ordering.end()}};
    // The first string of a font's own String INDEX is string 391.
    constexpr long kFirstOwnString = 391;
    std::vector<std::uint8_t> privateDict;
    (void)putNumber(privateDict, 0);
    putOperator(privateDict, 20); // defaultWidthX
    (void)putNumber(privateDict, 0);
    putOperator(privateDict, 21); // nominalWidthX

    std::vector<std::uint8_t> charset{2}; // Format 2: one range of CIDs from 1.
    if (glyphCount > 1) {
        put16(charset, 1);
        put16(charset, glyphCount - 2U);
    }
    std::vector<std::uint8_t> fdSelect{3};
    put16(fdSelect, 1);
    put16(fdSelect, 0);
    fdSelect.push_back(0);
    put16(fdSelect, glyphCount);

    const auto* matrix = find(*top, kOpFontMatrix);
    // The Top DICT's size does not depend on the offsets in it: each is five bytes.
    const auto topDict = [&](const std::uint32_t charsetAt, const std::uint32_t fdSelectAt,
                             const std::uint32_t charStringsOffset, const std::uint32_t fdArrayOffset) {
        std::vector<std::uint8_t> dict;
        (void)putNumber(dict, kFirstOwnString);
        (void)putNumber(dict, kFirstOwnString + 1);
        (void)putNumber(dict, 0);
        putOperator(dict, 0x0C1E); // ROS
        if (matrix != nullptr && matrix->operands.size() == 6) {
            dict.insert(dict.end(), matrix->raw.begin(), matrix->raw.end());
            putOperator(dict, kOpFontMatrix);
        }
        for (const long edge : {xMin, yMin, xMax, yMax})
            (void)putNumber(dict, edge);
        putOperator(dict, 5); // FontBBox
        putDictInteger(dict, glyphCount);
        putOperator(dict, 0x0C22); // CIDCount
        putDictInteger(dict, charsetAt);
        putOperator(dict, 15); // charset
        putDictInteger(dict, charStringsOffset);
        putOperator(dict, kOpCharStrings);
        putDictInteger(dict, fdArrayOffset);
        putOperator(dict, kOpFdArray);
        putDictInteger(dict, fdSelectAt);
        putOperator(dict, kOpFdSelect);
        return dict;
    };
    const auto fontDict = [&](const std::uint32_t privateAt) {
        std::vector<std::uint8_t> dict;
        putDictInteger(dict, static_cast<std::uint32_t>(privateDict.size()));
        putDictInteger(dict, privateAt);
        putOperator(dict, kOpPrivate);
        return dict;
    };
    const std::size_t topSize = topDict(0, 0, 0, 0).size();
    const std::size_t fontDictSize = fontDict(0).size();
    std::size_t at = 4 + indexSize(names) + (3 + 2 * 4 + topSize) + indexSize(strings) + 2;
    const auto charsetAt = static_cast<std::uint32_t>(at);
    at += charset.size();
    const auto fdSelectAt = static_cast<std::uint32_t>(at);
    at += fdSelect.size();
    const auto charStringsOffset = static_cast<std::uint32_t>(at);
    at += indexSize(charstrings);
    const auto fdArrayOffset = static_cast<std::uint32_t>(at);
    at += 3 + 2 * 4 + fontDictSize;
    const auto privateAt = static_cast<std::uint32_t>(at);

    std::vector<std::uint8_t> cff{1, 0, 4, 4};
    putIndex(cff, names);
    putIndex(cff, {topDict(charsetAt, fdSelectAt, charStringsOffset, fdArrayOffset)});
    putIndex(cff, strings);
    putIndex(cff, {}); // Global subroutines: none, every charstring is written out whole.
    cff.insert(cff.end(), charset.begin(), charset.end());
    cff.insert(cff.end(), fdSelect.begin(), fdSelect.end());
    putIndex(cff, charstrings);
    putIndex(cff, {fontDict(privateAt)});
    cff.insert(cff.end(), privateDict.begin(), privateDict.end());

    std::map<std::uint32_t, std::vector<std::uint8_t>> tables;
    static constexpr std::array<std::uint32_t, 7> dropped{
        makeTag("fvar"), makeTag("CFF2"), makeTag("avar"), makeTag("HVAR"),
        makeTag("VVAR"), makeTag("MVAR"), makeTag("VORG")};
    for (const auto& [tag, span] : sfnt.tables) {
        if (std::ranges::find(dropped, tag) != dropped.end())
            continue;
        tables[tag].assign(span.begin(), span.end());
    }
    tables[makeTag("CFF ")] = std::move(cff);
    tables[makeTag("hmtx")] = std::move(newHmtx);
    auto& newHhea = tables[makeTag("hhea")];
    newHhea[34] = static_cast<std::uint8_t>((glyphCount >> 8U) & 0xFFU);
    newHhea[35] = static_cast<std::uint8_t>(glyphCount & 0xFFU);
    if (anyOutline) {
        // The font's bounds follow the outlines.
        auto& newHead = tables[makeTag("head")];
        const std::array<long, 4> edges{xMin, yMin, xMax, yMax};
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto value = static_cast<std::uint16_t>(
                static_cast<std::int16_t>(std::clamp(edges[i], -32768L, 32767L)));
            newHead[36 + i * 2] = static_cast<std::uint8_t>(value >> 8U);
            newHead[37 + i * 2] = static_cast<std::uint8_t>(value & 0xFFU);
        }
    }
    return writeSfnt(makeTag("OTTO"), std::move(tables));
}

} // namespace bloom::runtime::fonts::detail
