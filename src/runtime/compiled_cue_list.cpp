// #752: the compiled cue list (compiled_value_graph.hpp, CompiledCueList).
#include <bloom/runtime/compiled_value_graph.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>

namespace bloom::runtime {
namespace {

class DigestWriter final {
  public:
    void add(const std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes[shift / 8] = static_cast<std::byte>((value >> shift) & 0xFFU);
        static_cast<void>(hasher_.update(bytes));
    }
    void add(const std::int64_t value) { add(static_cast<std::uint64_t>(value)); }
    void add(const double value) { add(std::bit_cast<std::uint64_t>(value)); }
    void add(const bool value) { add(std::uint64_t{value ? 1U : 0U}); }
    void add(const std::string_view text) {
        add(static_cast<std::uint64_t>(text.size()));
        static_cast<void>(hasher_.update(std::as_bytes(std::span(text.data(), text.size()))));
    }
    void add(const core::RationalTime time) {
        add(time.numerator());
        add(time.denominator());
    }
    template <typename T> void add(const std::optional<T>& value) {
        add(value.has_value());
        if (value)
            add(*value);
    }
    void add(const core::Color4d& color) {
        add(color.red);
        add(color.green);
        add(color.blue);
        add(color.alpha);
    }
    [[nodiscard]] core::Sha256Digest finish() const { return hasher_.finalize(); }

  private:
    core::Sha256Hasher hasher_;
};

} // namespace

core::Sha256Digest CompiledCueList::digestOf(const document::CueListValue& value) {
    DigestWriter out;
    out.add(std::string_view("BloomCueList1"));
    out.add(std::string_view(value.language));
    out.add(static_cast<std::uint64_t>(value.styles.size()));
    for (const auto& style : value.styles) {
        out.add(std::string_view(style.name));
        out.add(style.color);
        out.add(style.backgroundColor);
        out.add(style.outlineColor);
        out.add(style.outlineWidth);
        out.add(style.bold);
        out.add(style.italic);
        out.add(style.underline);
        out.add(style.fontFamily.has_value());
        if (style.fontFamily)
            out.add(std::string_view(*style.fontFamily));
        out.add(style.fontSize);
    }
    out.add(static_cast<std::uint64_t>(value.cues.size()));
    for (const auto& cue : value.cues) {
        out.add(cue->id);
        out.add(cue->start);
        out.add(cue->end);
        out.add(std::string_view(cue->text));
        out.add(std::string_view(cue->speaker));
        out.add(std::string_view(cue->style));
        const auto& placement = cue->placement;
        out.add(placement.anchor.has_value());
        if (placement.anchor)
            out.add(static_cast<std::uint64_t>(*placement.anchor));
        out.add(placement.positionX);
        out.add(placement.positionY);
        out.add(placement.lineOffset);
        out.add(placement.width);
        out.add(placement.textAlign.has_value());
        if (placement.textAlign)
            out.add(static_cast<std::uint64_t>(*placement.textAlign));
    }
    return out.finish();
}

core::Sha256Digest CompiledCueList::emptyDigest() {
    static const auto digest = digestOf(document::CueListValue{});
    return digest;
}

CompiledCueList::CompiledCueList(document::CueListValue value) {
    auto ends = std::vector<core::RationalTime>();
    ends.reserve(value.cues.size());
    for (const auto& cue : value.cues)
        ends.push_back(ends.empty() ? cue->end : std::max(ends.back(), cue->end));
    digest = digestOf(value);
    list = std::make_shared<const document::CueListValue>(std::move(value));
    runningEnd = std::make_shared<const std::vector<core::RationalTime>>(std::move(ends));
}

std::vector<std::size_t> CompiledCueList::activeAt(const core::RationalTime time) const {
    const auto& cues = list->cues;
    // The cues that start at or before `time`.
    const auto bound = std::upper_bound(cues.begin(), cues.end(), time,
                                        [](const core::RationalTime when,
                                           const document::CueHandle& cue) {
                                            return when < cue->start;
                                        });
    std::vector<std::size_t> active;
    for (auto index = static_cast<std::size_t>(bound - cues.begin()); index > 0; --index) {
        if (!(time < (*runningEnd)[index - 1]))
            break; // no cue at or before this one ends after `time`
        if (time < cues[index - 1]->end)
            active.push_back(index - 1);
    }
    std::ranges::reverse(active);
    return active;
}

} // namespace bloom::runtime
