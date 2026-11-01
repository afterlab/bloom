#include "generated_jpeg.hpp"
#include "packed_host_retirement_private.hpp"
#include <bloom/media/image.hpp>
#include <filesystem>
#include <fstream>

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using bloom::render::ImageWindow;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::GpuSceneCoverageCache;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

[[nodiscard]] std::shared_ptr<const Rgba32fImage> makeImage(const std::uint32_t width,
                                                            const std::uint32_t height) {
    const auto window = ImageWindow::create(0, 0, width, height);
    if (!window) {
        throw std::logic_error("cache test image window");
    }
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           bloom::core::PixelAspectRatio::square());
    if (!descriptor) {
        throw std::logic_error("cache test image descriptor");
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1U << 30U);
    if (!builder) {
        throw std::logic_error("cache test image builder");
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        throw std::logic_error("cache test image freeze");
    }
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

struct RetirementScope final {
    std::shared_ptr<bloom::render::PackedHostRetirementOwner> owner =
        bloom::render::PackedHostRetirementOwner::create();
    std::mutex mutex;
    std::condition_variable wake;
    bool blocked = false;
    bool entered = false;
    std::thread::id freeThread;
    RetirementScope() {
        auto state = owner->producerState();
        state->testContext = this;
        state->beforeFree = [](void* context) noexcept {
            auto& self = *static_cast<RetirementScope*>(context);
            std::unique_lock lock(self.mutex);
            self.entered = true;
            self.freeThread = std::this_thread::get_id();
            self.wake.notify_all();
            self.wake.wait(lock, [&] { return !self.blocked; });
        };
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            blocked = false;
        }
        wake.notify_all();
    }
    ~RetirementScope() {
        release();
        owner->beginShutdown();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        if (!owner->exitAcknowledged() || !owner->collect())
            std::terminate();
    }
};

template <class Predicate> bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    return predicate();
}

// A captured encoded still-source revision, the payload whose retention needs a fresh sample.
[[nodiscard]] bloom::media::ImageResult<std::shared_ptr<const bloom::media::ImageSourceRevision>>
captureRevision() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bloom-source-ticket-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jpg");
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(kGeneratedJpeg.data()), kGeneratedJpeg.size());
    }
    auto captured = bloom::media::captureImageSourceRevision(path, 1U << 20U);
    std::filesystem::remove(path);
    if (!captured.value.has_value())
        throw std::runtime_error("source revision retirement fixture capture");
    return captured;
}

void storeRevision(bloom::runtime::OperationCache& pool, const std::string& key,
                   const std::shared_ptr<const bloom::media::ImageSourceRevision>& revision) {
    pool.store(key, {}, {.image = {}, .values = {}, .bounds = {}, .sourceRevision = revision},
               bloom::runtime::OperationCacheEntryKind::DecodedMedia,
               pool.decodedMediaGeneration());
}

void testOrdinaryGraceIsPreserved(Expectations& checks) {
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    bloom::runtime::OperationCache pool(1U << 20U, ledger);
    const bloom::runtime::OperationCacheValue value{
        .image = {}, .values = {bloom::runtime::CompiledValue{0.5}}, .bounds = {}};
    pool.store("decoded", {}, value, bloom::runtime::OperationCacheEntryKind::DecodedMedia);
    pool.setByteBudget(pool.retainedBytes());
    pool.store("operation", {}, value);
    checks.expect(pool.find("decoded", {}).has_value() && !pool.find("operation", {}),
                  "ordinary-only incoming operation preserves an existing decoded grace entry");
}

// Staging is scratch (ADR 0037): the pool has one entry namespace and no prepared-upload kind.
// Binding a host owner roots it; rebinding the same owner is a no-op and another owner throws.
void testHostRetirementBinding(Expectations& checks) {
    RetirementScope scope;
    RetirementScope other;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    bloom::runtime::OperationCache pool(1U << 20U, ledger);
    bool missingRejected = false;
    try {
        pool.bindHostRetirement(nullptr);
    } catch (const std::invalid_argument&) {
        missingRejected = true;
    }
    pool.bindHostRetirement(scope.owner);
    pool.bindHostRetirement(scope.owner);
    bool otherRejected = false;
    try {
        pool.bindHostRetirement(other.owner);
    } catch (const std::invalid_argument&) {
        otherRejected = true;
    }
    checks.expect(missingRejected && otherRejected &&
                      pool.sourceRetirementState() == scope.owner->producerState(),
                  "one explicit host owner roots the pool; another owner is refused");
    pool.close();
}

void testClosedPoolStaysRooted(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    pool->bindHostRetirement(scope.owner);
    const auto image = makeImage(4, 4);
    pool->store("held", {}, {.image = image, .values = {}, .bounds = {}});
    pool->close();
    pool->store("held", {}, {.image = image, .values = {}, .bounds = {}});
    pool->store("later", {}, {.image = image, .values = {}, .bounds = {}});
    checks.expect(pool->sourceRetirementState() != nullptr && !pool->find("held", {}) &&
                      !pool->find("later", {}) && until([&] { return pool->retainedBytes() == 0; }),
                  "closed pool preserves its root binding and rejects later stores");
}

void testDeadlineExpiresWithoutDelivery(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    ledger.beginProcessSampling();
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    pool->bindHostRetirement(scope.owner);
    bloom::runtime::MemoryBudgetSample sample;
    sample.machine.availableBytes = 48ULL << 30U;
    sample.physicalBytes = 64ULL << 30U;
    sample.process.source = bloom::runtime::ProcessMemorySource::OtherHostOnly;
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unsupported;
    sample.process.sampledAt = std::chrono::steady_clock::now() - std::chrono::seconds(15) +
                               std::chrono::milliseconds(100);
    static_cast<void>(ledger.applySample(sample));
    const auto captured = captureRevision();
    storeRevision(*pool, "held", *captured.value);
    checks.expect(pool->find("held", {}).has_value(),
                  "delivered unexpired sample admits a rooted source revision");
    std::this_thread::sleep_until(sample.process.sampledAt + std::chrono::seconds(15) +
                                  std::chrono::milliseconds(1));
    storeRevision(*pool, "expired", *captured.value);
    checks.expect(!pool->find("expired", {}).has_value(),
                  "new stores check delivered expiry even without another ledger callback");
    pool->close();
}

void testSameLedgerFreshnessAndShrink(Expectations& checks) {
    RetirementScope scope;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    ledger.beginProcessSampling();
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    pool->bindHostRetirement(scope.owner);
    const auto captured = captureRevision();
    storeRevision(*pool, "cold", *captured.value);
    checks.expect(!pool->find("cold", {}).has_value() && pool->byteBudget() == 0,
                  "rooted cold delivery has no retention authority");
    bloom::runtime::MemoryBudgetSample sample;
    sample.machine.availableBytes = 48ULL << 30U;
    sample.physicalBytes = 64ULL << 30U;
    sample.process.source = bloom::runtime::ProcessMemorySource::OtherHostOnly;
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unsupported;
    sample.process.sampledAt = std::chrono::steady_clock::now();
    static_cast<void>(ledger.applySample(sample));
    const auto assigned = pool->byteBudget();
    storeRevision(*pool, "fresh", *captured.value);
    checks.expect(assigned > 256 && pool->find("fresh", {}).has_value(),
                  "actual same-ledger fresh retention admits a source revision");
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unavailable;
    sample.process.sampledAt = std::chrono::steady_clock::now();
    static_cast<void>(ledger.applySample(sample));
    storeRevision(*pool, "failed", *captured.value);
    checks.expect(!pool->find("failed", {}).has_value(),
                  "fresh timestamp on failed delivery does not grant new retention");
    sample.process.status = bloom::runtime::ProcessMemoryStatus::Unsupported;
    sample.process.sampledAt = std::chrono::steady_clock::now();
    static_cast<void>(ledger.applySample(sample));
    storeRevision(*pool, "recovered", *captured.value);
    checks.expect(pool->find("recovered", {}).has_value(),
                  "genuine fresh same-root recovery authorizes a new store");
    pool->setByteBudget(0);
    storeRevision(*pool, "tightened", *captured.value);
    checks.expect(pool->byteBudget() == 0 && !pool->find("tightened", {}).has_value() &&
                      until([&] { return pool->retainedBytes() == 0; }),
                  "serialized zero shrink removes entries without waiting under ledger lock");
}

void testSourceRevisionWorkerRetirement(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    pool->bindHostRetirement(scope.owner);
    auto captured = captureRevision();
    const std::weak_ptr<const bloom::media::ImageSourceRevision> backing = *captured.value;
    bool wrongKindRejected = false;
    try {
        pool->store("default-operation-source", {},
                    {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *captured.value});
    } catch (const std::invalid_argument&) {
        wrongKindRejected = true;
    }
    pool->clearOperations();
    checks.expect(wrongKindRejected && pool->retainedBytes() == 0 &&
                      pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) ==
                          0 &&
                      scope.owner->snapshot().allocations == 0,
                  "source revisions reject default Operation kind before registration/accounting");

    pool->store(
        "source", {}, {.image = {}, .values = {}, .bounds = {}, .sourceRevision = *captured.value},
        bloom::runtime::OperationCacheEntryKind::DecodedMedia, pool->decodedMediaGeneration());
    captured.value.reset();
    const auto charged = pool->retainedBytes();
    checks.expect(charged > kGeneratedJpeg.size() && scope.owner->snapshot().allocations == 1,
                  "rooted source ticket charges encoded backing/metadata and host registration");
    pool->setByteBudget(charged);
    const auto releasingThread = std::this_thread::get_id();
    pool->invalidateSourceRevisions();
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "source ticket reaches existing worker free gate");
    {
        const std::lock_guard lock(scope.mutex);
        checks.expect(scope.freeThread != releasingThread,
                      "source backing release runs off the purge/sampling caller thread");
    }
    checks.expect(
        !pool->find("source", {}) && !backing.expired() && pool->retainedBytes() == charged &&
            pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) == charged,
        "detached source remains charged and physically alive until worker free");
    pool->store("new-operation", {}, {.image = makeImage(4, 4), .values = {}, .bounds = {}});
    checks.expect(!pool->find("new-operation", {}) && pool->retainedBytes() == charged,
                  "pending source bytes cannot donate reusable admission capacity");
    pool->close();
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "source registration prevents shutdown exit while free is held");
    scope.release();
    checks.expect(until([&] { return backing.expired() && pool->retainedBytes() == 0; }),
                  "actual encoded backing free precedes source charge foldback");
}

void testOrdinaryPayloadWorkerRetirement(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
    pool->bindHostRetirement(scope.owner);
    auto image = makeImage(64, 64);
    const std::weak_ptr<const Rgba32fImage> backing = image;
    pool->store("ordinary", {}, {.image = image, .values = {}, .bounds = {}});
    image.reset();
    const auto ordinaryCharge = pool->retainedBytes();
    auto decoded = makeImage(64, 64);
    const std::weak_ptr<const Rgba32fImage> decodedBacking = decoded;
    pool->store("decoded", {}, {.image = decoded, .values = {}, .bounds = {}},
                bloom::runtime::OperationCacheEntryKind::DecodedMedia);
    decoded.reset();
    const auto totalCharge = pool->retainedBytes();
    const auto decodedCharge = totalCharge - ordinaryCharge;
    const auto caller = std::this_thread::get_id();
    pool->clearOperations();
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "ordinary CPU image reaches existing host worker free gate");
    {
        const std::lock_guard lock(scope.mutex);
        checks.expect(scope.freeThread != caller,
                      "ordinary image cache reference is released off the purge caller");
    }
    pool->clearDecodedMedia();
    checks.expect(!pool->find("ordinary", {}) && !pool->find("decoded", {}) && !backing.expired() &&
                      !decodedBacking.expired() && pool->retainedBytes() == totalCharge &&
                      pool->retainedBytes(bloom::runtime::OperationCacheEntryKind::DecodedMedia) ==
                          decodedCharge &&
                      scope.owner->snapshot().allocations == 2,
                  "ordinary and decoded misses leave both physical refs and exact charges pending");
    pool->close();
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "ordinary registration prevents early host-owner exit");
    scope.release();
    checks.expect(until([&] {
                      return backing.expired() && decodedBacking.expired() &&
                             pool->retainedBytes() == 0;
                  }),
                  "ordinary and decoded image frees precede worker charge foldback");
}

void testAccountingOutlivesPool(Expectations& checks) {
    RetirementScope scope;
    scope.blocked = true;
    bloom::runtime::MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    std::weak_ptr<bloom::runtime::OperationCache> weakPool;
    std::weak_ptr<const Rgba32fImage> weakImage;
    {
        auto pool = std::make_shared<bloom::runtime::OperationCache>(1U << 20U, ledger);
        weakPool = pool;
        pool->bindHostRetirement(scope.owner);
        auto image = makeImage(4, 4);
        weakImage = image;
        pool->store("sole", {}, {.image = image, .values = {}, .bounds = {}});
        image.reset();
    } // Last pool destruction itself enqueues the still-active entry without waiting.
    checks.expect(until([&] {
                      const std::lock_guard lock(scope.mutex);
                      return scope.entered;
                  }),
                  "last pool destruction enqueues reference release on the existing worker");
    checks.expect(
        weakPool.expired() && !weakImage.expired() && scope.owner->snapshot().allocations == 1,
        "retiring node owns neutral accounting without pinning the cache or freeing backing");
    scope.owner->beginShutdown();
    checks.expect(!scope.owner->exitAcknowledged(),
                  "cache destruction cannot falsely acknowledge the shared owner");
    scope.release();
    checks.expect(
        until([&] { return scope.owner->exitAcknowledged(); }) && weakImage.expired() &&
            scope.owner->snapshot().allocations == 0,
        "callback safely frees refs and neutral charge after pool destruction, then exits");
}

void testCoverageCacheEviction(Expectations& expectations) {
    // The coverage cache now retains immutable PathRasterCoverageGeometry (row ranges + spans), not
    // an R8 mask. Its accounting is the ACTUAL ranges + spans bytes.
    auto geometry = std::make_shared<bloom::runtime::GpuSceneCoverageGeometry>();
    geometry->width = 4;
    geometry->height = 4;
    geometry->rows.resize(std::size_t{4} * 4U);
    geometry->spans.resize(8U);
    const std::uint64_t bytes = bloom::runtime::gpuSceneCoverageGeometryBytes(*geometry);
    expectations.expect(bytes == 16U * sizeof(bloom::render::PathRasterCoverageRange) +
                                     8U * sizeof(bloom::render::PathRasterCoverageSpan),
                        "the charged coverage bytes are the actual row ranges + spans");

    auto cache = std::make_shared<GpuSceneCoverageCache>(2U * bytes);
    cache->store("a", geometry);
    cache->store("b", geometry);
    expectations.expect(cache->entryCount() == 2 && cache->retainedBytes() == 2U * bytes,
                        "two equal geometries are retained at the exact byte accounting");
    cache->store("c", geometry);
    expectations.expect(cache->retainedBytes() <= 2U * bytes && cache->entryCount() == 2,
                        "the coverage cache byte ceiling is never exceeded across eviction");
    expectations.expect(cache->find("c") != nullptr && cache->find("c").get() == geometry.get(),
                        "the most recent coverage entry serves the shared geometry identity");
    expectations.expect(cache->find("a") == nullptr && cache->find("b") != nullptr,
                        "the least-recently-used coverage entry was evicted");
    // Re-storing the same identity under an existing key replaces in place with no extra bytes.
    cache->store("c", geometry);
    expectations.expect(cache->retainedBytes() == 2U * bytes && cache->entryCount() == 2,
                        "replacing an existing key keeps the byte accounting unchanged");
    expectations.expect(cache->find("c").get() == geometry.get(),
                        "the replaced entry still serves the shared geometry identity");
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testCoverageCacheEviction(expectations);
        testOrdinaryGraceIsPreserved(expectations);
        testHostRetirementBinding(expectations);
        testOrdinaryPayloadWorkerRetirement(expectations);
        testSourceRevisionWorkerRetirement(expectations);
        testSameLedgerFreshnessAndShrink(expectations);
        testDeadlineExpiresWithoutDelivery(expectations);
        testClosedPoolStaysRooted(expectations);
        testAccountingOutlivesPool(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: operation cache host retirement expectations failed\n";
            return 1;
        }
        std::cout << "PASS: operation cache host retirement\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
