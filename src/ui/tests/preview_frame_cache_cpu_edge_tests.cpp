// CPU-only edge tests for the RAM preview cache, independent of any GPU device. They build genuine
// CPU reference display frames with the real CpuCompositionEvaluator + CpuReferenceDisplayPreparer
// and exercise:
//   * CACHEFIX-2 same-key CPU recovery/reuse: a live frame with an already-retained key is reused
//     (one entry, no double byte charge, generation re-stamped on take);
//   * CACHEFIX-3 other-project retention: a standalone cache with no provenance policy keeps
//     another project's entries, while a same-project older revision is still dropped;
//   * dead-entry visibility is a resident concern (covered by the device test).
// It never creates a GpuDevice and never touches Vulkan.

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
using bloom::core::Color4d;
using bloom::runtime::PreparedPreviewFrame;
using bloom::runtime::PreviewRequestIdentity;
using bloom::ui::PreviewFrameCache;
using bloom::ui::PreviewFrameCacheKey;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (!condition) {
            ++failures_;
            std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = document::NodeId::fromRaw(10);
constexpr auto kLayerNode = document::NodeId::fromRaw(11);
constexpr auto kStackNode = document::NodeId::fromRaw(12);
constexpr auto kOutputNode = document::NodeId::fromRaw(13);
constexpr auto kLayer = document::LayerId::fromRaw(20);
constexpr auto kSlot = document::LayerSlotId::fromRaw(30);

[[nodiscard]] document::CompositionFormat format() {
    const auto value = document::CompositionFormat::create(64, 36);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneSolidPlan(const document::ProjectId projectId = kProjectId,
             const document::Revision revision = document::Revision::fromRaw(7),
             const Color4d color = Color4d{1.0, 0.0, 0.0, 1.0}) {
    const auto compositionFormat = format();
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), color},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                                static_cast<double>(compositionFormat.width())},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                                static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNode, kLayer, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(41),
                                       document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(43),
                                       document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(44), document::kDefaultScale},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(45),
                                         document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(42), 1.0},
        document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode,
        {runtime::CompiledMergeInput{kSlot, kLayer, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{revision, projectId, kCompositionId,
                                                   compositionFormat, std::move(operations),
                                                   runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
cpuFrame(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const std::uint64_t generation, const bool allowRgb10A2 = false) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(
        plan,
        runtime::EvaluationRequest{.time = bloom::core::RationalTime::fromInteger(0),
                                   .output = plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = std::size_t{1} << 22},
        {});
    if (evaluated.frame() == nullptr) {
        return nullptr;
    }
    const runtime::CpuReferenceDisplayPreparer preparer;
    const auto display =
        preparer.prepare(evaluated.frame(),
                         runtime::ReferenceDisplayPreparationRequest{
                             .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                             .aggregatePixelStorageByteLimit = std::size_t{1} << 22,
                             .viewAdjust = {},
                             .displayName = {},
                             .viewName = {},
                             .showLook = true,
                             .allowRgb10A2 = allowRgb10A2},
                         {});
    if (display.frame() == nullptr) {
        return nullptr;
    }
    const auto prepared = PreparedPreviewFrame::create(generation, display.frame());
    if (!prepared.has_value()) {
        return nullptr;
    }
    return std::make_shared<const PreparedPreviewFrame>(*prepared);
}

// #730, #733 Scopes: a cached frame stores the Working-signal tables its preparation counted,
// charged to the host byte budget, and never Display-signal tables (those are counted from the
// frame a Viewer shows, at display time). Tables never decide a hit: a request that asks for
// tables the cached frame lacks is handed the frame as it is.
void testNoDisplayTablesStored(Expectations& expectations,
                               const std::shared_ptr<const PreparedPreviewFrame>& frameA1,
                               const PreviewFrameCacheKey& keyA, const std::size_t budget) {
    runtime::ScopeMeasureSettings display;
    runtime::ScopeMeasureSettings working;
    working.signal = runtime::ScopeSignal::Working;
    runtime::ScopeMeasureSettings workingWide = working;
    workingWide.axisMaximum = 4;
    const auto tablesFor = [](const runtime::ScopeMeasureSettings& settings,
                              const render::ScopeTableReduction reduction = {}) {
        const auto pixel = render::Rgba32f::fromPremultiplied(0.5F, 0.5F, 0.5F, 1.0F);
        const std::vector<render::Rgba32f> gray(2, *pixel.value());
        runtime::ScopeMeasureInput input;
        input.signal = gray;
        input.width = 2;
        input.height = 1;
        input.areaWidth = 2;
        input.areaHeight = 1;
        input.scopes = render::kScopeTableHistogram | render::kScopeTableWaveform;
        input.axisMaximum = settings.axisMaximum;
        input.luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
        input.reduction = reduction;
        auto scopes = std::make_shared<runtime::PreviewFrameScopes>();
        scopes->settings = settings;
        scopes->reduction = reduction;
        scopes->status = runtime::PreviewFrameScopesStatus::Measured;
        if (const auto counts = runtime::measureScopesCpu(input))
            scopes->tables = runtime::ScopeTables::pack(*counts, runtime::ScopeRoute::Cpu);
        scopes->area = {0, 0, 2, 1};
        return std::shared_ptr<const runtime::PreviewFrameScopes>(std::move(scopes));
    };
    const auto requestFor = [](const runtime::ScopeMeasureSettings& settings,
                               const render::ScopeTableReduction reduction = {}) {
        return runtime::PreviewScopeRequest{
            .settings = settings, .reduction = reduction, .colors = {}, .ocio = {}};
    };
    const auto asking = [&](const runtime::ScopeMeasureSettings& settings) {
        auto identity = frameA1->desiredIdentity();
        identity.scopes =
            std::make_shared<const runtime::PreviewScopeRequest>(requestFor(settings));
        return identity;
    };
    const auto carrying = [&](std::shared_ptr<const runtime::PreviewFrameScopes> tables) {
        return std::make_shared<const PreparedPreviewFrame>(frameA1->withScopes(std::move(tables)));
    };
    PreviewFrameCache scopeCache(budget);
    expectations.expect(scopeCache.insert(frameA1).retained() && scopeCache.scopeTableBytes() == 0,
                        "scopes: the frame is cached without tables");
    expectations.expect(scopeCache.take(frameA1->desiredIdentity()) != nullptr,
                        "scopes: a request that asks for no tables hits");
    {
        const auto misses = scopeCache.statistics().misses;
        const auto hit = scopeCache.take(asking(working));
        expectations.expect(hit != nullptr && hit->scopes() == nullptr &&
                                scopeCache.statistics().misses == misses &&
                                !scopeCache.containsWithScopes(keyA, requestFor(working)),
                            "scopes: a request that asks for tables hits a frame without them");
    }

    // Display-signal tables are never stored: the frame is kept as it is.
    const auto displayTables = tablesFor(display);
    const auto hostWithout = scopeCache.hostBytes();
    expectations.expect(scopeCache.insert(carrying(displayTables)).outcome ==
                                PreviewFrameCache::InsertOutcome::AlreadyPresent &&
                            scopeCache.size() == 1 && scopeCache.scopesOf(keyA) == nullptr &&
                            scopeCache.scopeTableBytes() == 0 &&
                            scopeCache.hostBytes() == hostWithout,
                        "scopes: Display tables are not stored and not charged");
    {
        PreviewFrameCache fresh(budget);
        expectations.expect(fresh.insert(carrying(displayTables)).retained() &&
                                fresh.scopesOf(keyA) == nullptr && fresh.scopeTableBytes() == 0,
                            "scopes: a new entry of a frame with Display tables stores none");
        const auto taken = fresh.take(frameA1->desiredIdentity());
        expectations.expect(taken != nullptr && taken->scopes() == nullptr,
                            "scopes: the frame handed back carries no Display tables");
    }

    // Working-signal tables are stored with the frame and charged.
    const auto workingTables = tablesFor(working);
    expectations.expect(scopeCache.insert(carrying(workingTables)).outcome ==
                                PreviewFrameCache::InsertOutcome::Retained &&
                            scopeCache.size() == 1,
                        "scopes: the frame with its tables replaces the frame without");
    expectations.expect(scopeCache.scopeTableBytes() == workingTables->byteSize() &&
                            scopeCache.hostBytes() == hostWithout + workingTables->byteSize(),
                        "scopes: the tables are charged to the host byte budget");
    const auto hit = scopeCache.take(asking(working));
    expectations.expect(hit != nullptr && hit->scopes() == workingTables &&
                            scopeCache.containsWithScopes(keyA, requestFor(working)),
                        "scopes: a request for the same settings hits with the tables");
    {
        // Other settings, and the same settings at another table size, still hit: the frame is
        // handed with the tables it has, and the caller counts the others in the background.
        const auto misses = scopeCache.statistics().misses;
        const auto other = scopeCache.take(asking(workingWide));
        expectations.expect(
            other != nullptr && other->scopes() == workingTables &&
                scopeCache.statistics().misses == misses &&
                !scopeCache.containsWithScopes(keyA, requestFor(workingWide)) &&
                !scopeCache.containsWithScopes(
                    keyA, requestFor(working, render::ScopeTableReduction{1, 1, 1, 1})),
            "scopes: tables of other settings or another size never make a miss");
    }
    // The frame cached again without tables, or with Display tables, keeps the ones it carries.
    const auto statistics = scopeCache.statistics();
    expectations.expect(scopeCache.insert(frameA1).outcome ==
                                PreviewFrameCache::InsertOutcome::AlreadyPresent &&
                            scopeCache.insert(carrying(displayTables)).outcome ==
                                PreviewFrameCache::InsertOutcome::AlreadyPresent &&
                            scopeCache.scopesOf(keyA) == workingTables,
                        "scopes: a frame without stored tables does not drop the cached ones");
    expectations.expect(scopeCache.statistics().hits == statistics.hits &&
                            scopeCache.statistics().misses == statistics.misses,
                        "scopes: reading the tables counts no hit or miss");
    // Tables of other settings, and of another size, replace the cached ones.
    const auto wideTables = tablesFor(workingWide);
    (void)scopeCache.insert(carrying(wideTables));
    expectations.expect(scopeCache.scopesOf(keyA) == wideTables &&
                            scopeCache.containsWithScopes(keyA, requestFor(workingWide)) &&
                            !scopeCache.containsWithScopes(keyA, requestFor(working)) &&
                            scopeCache.scopeTableBytes() == wideTables->byteSize(),
                        "scopes: tables of other settings replace the cached ones");
    const render::ScopeTableReduction smaller{0, 2, 0, 0};
    const auto smallTables = tablesFor(workingWide, smaller);
    (void)scopeCache.insert(carrying(smallTables));
    expectations.expect(scopeCache.scopesOf(keyA) == smallTables &&
                            scopeCache.containsWithScopes(keyA, requestFor(workingWide, smaller)) &&
                            !scopeCache.containsWithScopes(keyA, requestFor(workingWide)) &&
                            scopeCache.scopeTableBytes() == smallTables->byteSize() &&
                            smallTables->byteSize() < wideTables->byteSize(),
                        "scopes: tables of another size replace the cached ones");
    scopeCache.clear();
    expectations.expect(scopeCache.scopeTableBytes() == 0,
                        "scopes: a cleared cache holds no tables");
}

// #733: a cached display frame is four bytes a pixel in either layout, and the frame counters
// (what the packed pixels cannot carry) travel with it: nine 32-bit counts, charged with the frame
// and handed back by the cache.
void testDisplayFrameBytesAndCounters(Expectations& expectations, const std::size_t budget) {
    // A solid brighter than the display's white in red: every covered pixel is over the axis.
    const auto plan =
        oneSolidPlan(kProjectId, document::Revision::fromRaw(9), Color4d{2.0, 0.25, 0.0, 1.0});
    const auto wide = cpuFrame(plan, 1, true);
    const auto straight = cpuFrame(plan, 1, false);
    expectations.expect(wide != nullptr && straight != nullptr, "counters: the frames are built");
    if (wide == nullptr || straight == nullptr) {
        return;
    }
    const auto wideView = wide->displayBufferView();
    const auto straightView = straight->displayBufferView();
    expectations.expect(wideView.has_value() && straightView.has_value() &&
                            wideView->pixelLayout == render::DisplayPixelLayout::Rgb10A2 &&
                            straightView->pixelLayout == render::DisplayPixelLayout::Straight8888,
                        "counters: a frame of opaque and transparent pixels is 10:10:10:2 when "
                        "its producer allows it, straight otherwise");
    if (!wideView.has_value() || !straightView.has_value()) {
        return;
    }
    const std::size_t pixels = std::size_t{format().width()} * format().height();
    expectations.expect(wideView->pixels.size() == pixels &&
                            wideView->pixels.size_bytes() == pixels * 4 &&
                            straightView->pixels.size_bytes() == pixels * 4,
                        "counters: either layout is four bytes a pixel");
    std::uint32_t covered = 0;
    bool codesMatch = true;
    for (std::size_t index = 0; index < pixels; ++index) {
        const auto codes = render::rgb10A2Codes(wideView->pixels[index]);
        const bool opaque = straightView->pixels[index].alpha == 255;
        covered += opaque ? 1U : 0U;
        codesMatch = codesMatch &&
                     (opaque ? codes.alpha == render::kDisplayAlpha2Opaque &&
                                   codes.red == render::kDisplayCode10Maximum && codes.blue == 0
                             : render::displayWord(wideView->pixels[index]) == 0U);
    }
    expectations.expect(covered > 0 && codesMatch,
                        "counters: the 10-bit words hold the same picture as the straight frame");
    const auto counters = wide->displayCounters();
    expectations.expect(counters.has_value() && counters == straight->displayCounters(),
                        "counters: the layout does not change the frame counters");
    if (!counters.has_value()) {
        return;
    }
    const render::DisplayFrameCounters expected{.under = {0, 0, 0, 0},
                                                .over = {covered, 0, 0, 0},
                                                .notFinite = 0};
    expectations.expect(*counters == expected && counters->lumaCounted(),
                        "counters: every covered pixel is over the axis in red, and nothing else");

    static_assert(sizeof(render::DisplayFrameCounters) == 36);
    const auto cost = PreviewFrameCache::frameByteCost(*wide);
    expectations.expect(cost == pixels * 4 + wide->evaluatedBounds().size_bytes() +
                                    sizeof(render::DisplayFrameCounters) &&
                            cost == PreviewFrameCache::frameByteCost(*straight),
                        "counters: a frame is charged its pixels, its geometry and the 36 bytes of "
                        "its counters, the same in either layout");
    PreviewFrameCache cache(budget);
    expectations.expect(cache.insert(wide).retained() && cache.hostBytes() == cost,
                        "counters: the cache charges exactly that");
    const auto taken = cache.take(wide->desiredIdentity());
    const auto takenView = taken != nullptr ? taken->displayBufferView() : std::nullopt;
    expectations.expect(taken != nullptr && !taken->hasProcessFrame() && takenView.has_value() &&
                            takenView->pixelLayout == render::DisplayPixelLayout::Rgb10A2 &&
                            std::equal(takenView->pixels.begin(), takenView->pixels.end(),
                                       wideView->pixels.begin(), wideView->pixels.end()) &&
                            taken->displayCounters() == counters &&
                            PreviewFrameCache::frameByteCost(*taken) == cost,
                        "counters: the cached copy keeps the layout, every word and the counters");
}

} // namespace

int main(int argc, char** argv) {
    try {
        QCoreApplication application(argc, argv);
        Expectations expectations;
        constexpr std::size_t kBudget = std::size_t{1} << 28;

        auto planA = oneSolidPlan();
        auto frameA1 = cpuFrame(planA, 1);
        expectations.expect(frameA1 != nullptr && frameA1->displayBufferView().has_value(),
                            "the CPU reference frame is built with a display buffer");
        if (frameA1 == nullptr) {
            return 1;
        }
        const auto keyA = PreviewFrameCacheKey::forIdentity(frameA1->desiredIdentity());
        const auto costA = PreviewFrameCache::frameByteCost(*frameA1);

        // G0: the trace must report real retention decisions, including a refused zero-budget
        // insert. A returned void insert() is never treated as proof a frame was retained.
        auto trace = std::make_shared<bloom::ui::PreviewFrameTrace>(32);
        PreviewFrameCache zeroCache(0);
        zeroCache.setFrameTrace(trace);
        const auto zeroResult = zeroCache.insert(frameA1);
        expectations.expect(zeroResult.outcome == PreviewFrameCache::InsertOutcome::RejectedBudget,
                            "zero budget returns explicit refusal");
        expectations.expect(zeroCache.size() == 0 && trace->events().size() == 1 &&
                                trace->events().back().outcome ==
                                    bloom::ui::PreviewTraceOutcome::OverBudget,
                            "G0: zero-budget refusal is traced honestly");
        zeroCache.setByteBudget(kBudget);
        expectations.expect(zeroCache.insert(frameA1).outcome ==
                                PreviewFrameCache::InsertOutcome::Retained,
                            "adequate budget reports actual retention");
        expectations.expect(zeroCache.contains(keyA) &&
                                trace->events().back().outcome ==
                                    bloom::ui::PreviewTraceOutcome::Retained,
                            "G0: retained means a live matching entry actually exists");
        auto tracedIdentity = frameA1->desiredIdentity();
        tracedIdentity.requestGeneration = 9;
        const auto tracedHit = zeroCache.take(tracedIdentity);
        expectations.expect(tracedHit && trace->events().back().identity.generation == 9 &&
                                trace->events().back().outcome ==
                                    bloom::ui::PreviewTraceOutcome::Hit,
                            "G0: a warm cache hit records the new publication generation");
        zeroCache.setByteBudget(0);
        expectations.expect(trace->events().back().outcome ==
                                bloom::ui::PreviewTraceOutcome::Removed,
                            "G0: removal is distinct from admission");

        // CACHEFIX-2 CPU path: a live frame with an already-retained key is reused in place, not
        // re-charged. This is the branch the dead-entry fix must not disturb.
        PreviewFrameCache reuseCache(kBudget);
        reuseCache.insert(frameA1);
        expectations.expect(reuseCache.size() == 1 && reuseCache.residentBytes() == costA,
                            "the first CPU insert retains exactly one charged entry");
        auto frameA2 = cpuFrame(planA, 2);
        expectations.expect(frameA2 != nullptr, "the second same-key CPU frame is built");
        if (frameA2 != nullptr) {
            expectations.expect(reuseCache.insert(frameA2).outcome ==
                                    PreviewFrameCache::InsertOutcome::AlreadyPresent,
                                "a live duplicate reports reuse");
            expectations.expect(reuseCache.size() == 1,
                                "CACHEFIX-2: a live same-key CPU insert keeps one entry");
            expectations.expect(reuseCache.residentBytes() == costA,
                                "CACHEFIX-2: a live same-key CPU insert does not double-charge");
            expectations.expect(reuseCache.contains(keyA),
                                "CACHEFIX-2: contains() still sees the reused CPU entry");
            auto identity = frameA1->desiredIdentity();
            identity.requestGeneration = 3;
            auto taken = reuseCache.take(identity);
            expectations.expect(taken != nullptr && taken->desiredIdentity().requestGeneration == 3,
                                "CACHEFIX-2: take() re-stamps the reused CPU frame's generation");
            expectations.expect(reuseCache.statistics().hits == 1,
                                "CACHEFIX-2: the reused CPU take counts a hit");
        }

        // CACHEFIX-3: a standalone cache with no provenance policy must retain another project's
        // entries. The pre-fix dropStaleRevisions() dropped every entry with a different projectId.
        auto planB = oneSolidPlan(document::ProjectId::fromRaw(999));
        auto frameB = cpuFrame(planB, 1);
        expectations.expect(frameB != nullptr, "the second project's CPU frame is built");
        if (frameB == nullptr) {
            return 1;
        }
        PreviewFrameCache projectCache(kBudget);
        projectCache.insert(frameA1);
        projectCache.insert(frameB);
        expectations.expect(projectCache.size() == 2,
                            "CACHEFIX-3: both projects' CPU frames are retained together");
        expectations.expect(projectCache.contains(keyA),
                            "CACHEFIX-3: a second project's insert preserves the first project");
        expectations.expect(projectCache.timesFor(keyA).size() == 1,
                            "CACHEFIX-3: timesFor() still lists the first project's entry");

        // The fix must NOT weaken same-project revision invalidation: an older-revision entry of
        // the SAME project is still dropped when a newer revision arrives.
        auto planA2 = oneSolidPlan(kProjectId, document::Revision::fromRaw(8));
        auto frameA2rev = cpuFrame(planA2, 1);
        expectations.expect(frameA2rev != nullptr, "the newer same-project CPU frame is built");
        if (frameA2rev != nullptr) {
            PreviewFrameCache revisionCache(kBudget);
            revisionCache.insert(frameA1);
            for (const auto boundary : {PreviewFrameCache::InsertAllocation::Key,
                                        PreviewFrameCache::InsertAllocation::Product,
                                        PreviewFrameCache::InsertAllocation::Capacity}) {
                revisionCache.failNextInsertAllocationForTesting(boundary);
                const auto failed = revisionCache.insert(frameA2rev);
                expectations.expect(
                    failed.outcome == PreviewFrameCache::InsertOutcome::AllocationFailure &&
                        revisionCache.contains(keyA) && revisionCache.size() == 1 &&
                        revisionCache.residentBytes() == costA &&
                        revisionCache.statistics().staleDrops == 0,
                    "all allocation boundaries preserve the old revision and charge");
            }
            const auto staleBefore = revisionCache.statistics().staleDrops;
            revisionCache.insert(frameA2rev);
            expectations.expect(revisionCache.size() == 1 &&
                                    revisionCache.statistics().staleDrops == staleBefore + 1,
                                "CACHEFIX-3: same-project older revision is still dropped");
            expectations.expect(!revisionCache.contains(keyA),
                                "CACHEFIX-3: the superseded same-project entry is gone");
        }

        // Preview first (#413): during a RAM Preview fill a frame that does not fit takes room
        // from an Ordinary pool instead of evicting an earlier finished frame; outside a fill the
        // cache evicts its own oldest frame as before.
        {
            const auto costB = PreviewFrameCache::frameByteCost(*frameB);
            runtime::MemoryBudgetLedger ledger(std::size_t{64} << 30, std::size_t{64} << 30);
            ledger.setRamHeadroom(std::size_t{2} << 30);
            std::size_t intermediates = std::size_t{1} << 20;
            ledger.registerCache(
                &intermediates, std::size_t{1} << 20, [&] { return intermediates; },
                [&](const std::size_t bytes) { intermediates = std::min(intermediates, bytes); });
            {
                PreviewFrameCache outside(costA + costB - 1, ledger);
                outside.insert(frameA1);
                outside.insert(frameB);
                expectations.expect(outside.size() == 1 && !outside.contains(keyA) &&
                                        intermediates == std::size_t{1} << 20 &&
                                        outside.statistics().previewFirstYieldedBytes == 0,
                                    "outside a fill the preview cache evicts its own oldest frame");
            }
            {
                PreviewFrameCache filling(costA + costB - 1, ledger);
                auto fill = filling.beginFillRetention();
                filling.insert(frameA1);
                const auto inserted = filling.insert(frameB);
                expectations.expect(
                    inserted.retained() && inserted.evictions == 0 && filling.size() == 2 &&
                        filling.contains(keyA) && intermediates < std::size_t{1} << 20 &&
                        filling.statistics().previewFirstYieldedBytes ==
                            (std::size_t{1} << 20) - intermediates &&
                        filling.byteBudget() >= costA + costB,
                    "during a fill an Ordinary pool yields before a finished frame is evicted");
                fill.end();
                expectations.expect(!ledger.previewFillActive(),
                                    "ending the fill restores normal yielding");
            }
            ledger.unregisterCache(&intermediates);
        }

        // #462: a budget the ledger applies under its lock is announced after the ledger returns,
        // so an observer that reads the ledger back (the status bar, the background fill) never
        // runs inside its lock. A burst of changes is announced once.
        {
            runtime::MemoryBudgetLedger ledger(std::size_t{64} << 30, std::size_t{64} << 30);
            PreviewFrameCache cache(std::size_t{1} << 20, ledger);
            int announced = 0;
            bool insideLedger = false;
            std::future<runtime::MemoryBudgetState> probe;
            QObject::connect(&cache, &PreviewFrameCache::byteBudgetChanged, [&] {
                if (++announced != 1)
                    return;
                probe = std::async(std::launch::async, [&ledger] { return ledger.state(); });
                insideLedger = probe.wait_for(std::chrono::seconds(2)) != std::future_status::ready;
            });
            cache.setByteBudget(std::size_t{2} << 20);
            cache.setByteBudget(std::size_t{3} << 20);
            QCoreApplication::sendPostedEvents(&cache);
            if (probe.valid())
                static_cast<void>(probe.get());
            expectations.expect(announced == 1 && !insideLedger &&
                                    cache.byteBudget() == std::size_t{3} << 20,
                                "a ledger-applied budget is announced once, outside its lock");
        }

        // #728: a budget another thread asks the ledger for (an export's capacity sample) is not
        // applied on that thread. The cache keeps its entries until its own thread takes the
        // budget, and inserting while the other thread works touches nothing they share.
        {
            runtime::MemoryBudgetLedger ledger(std::size_t{64} << 30, std::size_t{64} << 30);
            PreviewFrameCache cache(std::size_t{1} << 28, ledger);
            cache.insert(frameA1);
            auto elsewhere = std::async(std::launch::async, [&ledger, &cache] {
                static_cast<void>(ledger.setCacheCeiling(&cache, 0));
            });
            elsewhere.get();
            expectations.expect(cache.size() == 1 && cache.contains(keyA) &&
                                    cache.byteBudget() == std::size_t{1} << 28,
                                "a budget applied from another thread leaves the cache alone");
            QCoreApplication::sendPostedEvents(&cache);
            expectations.expect(cache.size() == 0 && cache.byteBudget() == 0,
                                "the cache's own thread applies the pending budget");
            // The same with no ordering between the two threads: under the thread sanitizer any
            // shared access between the inserts and the other thread's budgets is reported.
            elsewhere = std::async(std::launch::async, [&ledger, &cache] {
                for (std::size_t round = 1; round <= 8; ++round)
                    static_cast<void>(ledger.setCacheCeiling(&cache, round << 24));
            });
            for (std::uint64_t generation = 2; generation < 10; ++generation)
                cache.insert(cpuFrame(planA, generation));
            elsewhere.get();
            QCoreApplication::sendPostedEvents(&cache);
            expectations.expect(cache.byteBudget() == std::size_t{8} << 24,
                                "the latest budget asked from another thread is the one applied");
        }

        // ADR 0037 rule 3: the source working set releases decoded sources outside the playback
        // window; finished frames are not part of it. Playback far ahead of a cached frame, with
        // every request ending and its release pass running, leaves the finished frame cached.
        {
            runtime::MemoryBudgetLedger ledger(std::size_t{16} << 30U);
            PreviewFrameCache finished(kBudget, ledger);
            runtime::OperationCache operations(kBudget, ledger);
            expectations.expect(finished.insert(frameA1).retained(), "a finished frame is cached");
            const auto window = operations.sourceWindow();
            for (std::int64_t frame = 0; frame < 200; ++frame) {
                const runtime::SourceWorkingSet::Request request(window, kCompositionId.value(),
                                                                 frame);
                operations.store("source:" + std::to_string(frame), {},
                                 {.image = {}, .values = {1.0}, .bounds = {}},
                                 runtime::OperationCacheEntryKind::DecodedMedia);
            }
            expectations.expect(
                operations.statistics().hits == 0 &&
                    operations.retainedBytes(runtime::OperationCacheEntryKind::DecodedMedia) > 0,
                "sources inside the window stay");
            expectations.expect(finished.size() == 1 && finished.contains(keyA) &&
                                    finished.statistics().evictions == 0,
                                "finished frames are untouched by the source working set");
        }

        testNoDisplayTablesStored(expectations, frameA1, keyA, kBudget);
        testDisplayFrameBytesAndCounters(expectations, kBudget);

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " cache CPU edge expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: preview frame cache CPU edge cases\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected CPU edge test exception: " << exception.what() << '\n';
        return 1;
    }
}
