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
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
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
             const document::Revision revision = document::Revision::fromRaw(7)) {
    const auto compositionFormat = format();
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), Color4d{1.0, 0.0, 0.0, 1.0}},
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
         const std::uint64_t generation) {
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
                             .showLook = true},
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

        // #724 Scopes: a sidecar's identity names the picture it counted. A Retained sidecar never
        // answers a Rendered lookup and the reverse; the entry keeps one sidecar at a time.
        {
            PreviewFrameCache scopeCache(kBudget);
            expectations.expect(scopeCache.insert(frameA1).retained(),
                                "scope identity: the frame is cached");
            bloom::ui::PreviewScopeIdentity rendered;
            bloom::ui::PreviewScopeIdentity retained;
            retained.source = runtime::ScopeSource::Retained;
            expectations.expect(rendered.source == runtime::ScopeSource::Rendered &&
                                    !(rendered == retained),
                                "scope identity: Rendered is the default and differs from Retained");
            auto renderedSidecar = std::make_shared<bloom::ui::PreviewScopeSidecar>();
            renderedSidecar->identity = rendered;
            auto retainedSidecar = std::make_shared<bloom::ui::PreviewScopeSidecar>();
            retainedSidecar->identity = retained;
            expectations.expect(scopeCache.attachScopes(keyA, renderedSidecar),
                                "scope identity: a Rendered sidecar attaches");
            expectations.expect(scopeCache.scopesFor(keyA, rendered) == renderedSidecar &&
                                    scopeCache.scopesFor(keyA, retained) == nullptr,
                                "scope identity: a Rendered sidecar never satisfies a Retained "
                                "lookup");
            expectations.expect(scopeCache.attachScopes(keyA, retainedSidecar),
                                "scope identity: a Retained sidecar attaches");
            expectations.expect(scopeCache.scopesFor(keyA, retained) == retainedSidecar &&
                                    scopeCache.scopesFor(keyA, rendered) == nullptr,
                                "scope identity: a Retained sidecar never satisfies a Rendered "
                                "lookup");
        }

        // #724 Scopes: peekScopeFrame hands a measure the retained frame and its source without
        // counting a hit or a miss and without moving the entry in the eviction order.
        {
            auto planC = oneSolidPlan(document::ProjectId::fromRaw(998));
            auto frameC = cpuFrame(planC, 1);
            expectations.expect(frameC != nullptr, "peek: the third frame is built");
            if (frameC != nullptr) {
                const auto keyB = PreviewFrameCacheKey::forIdentity(frameB->desiredIdentity());
                // Room for two frames, not three.
                PreviewFrameCache peekCache(costA * 2 + costA / 2);
                expectations.expect(peekCache.insert(frameA1).retained() &&
                                        peekCache.insert(frameB).retained(),
                                    "peek: two frames are cached, the older one is A");
                const auto before = peekCache.statistics();
                const auto peeked = peekCache.peekScopeFrame(keyA);
                expectations.expect(peeked && peeked->frame != nullptr &&
                                        peeked->frame->displayBufferView().has_value() &&
                                        peeked->source.identity == frameA1->desiredIdentity(),
                                    "peek: the handle and the source of the entry");
                expectations.expect(!peekCache.peekScopeFrame(
                                        PreviewFrameCacheKey::forIdentity(frameC->desiredIdentity())),
                                    "peek: no entry, nothing");
                const auto after = peekCache.statistics();
                expectations.expect(after.hits == before.hits && after.misses == before.misses,
                                    "peek: no hit or miss is counted");
                expectations.expect(peekCache.insert(frameC).retained() &&
                                        !peekCache.contains(keyA) && peekCache.contains(keyB),
                                    "peek: the peeked entry is still the oldest and is evicted");
            }
        }

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
