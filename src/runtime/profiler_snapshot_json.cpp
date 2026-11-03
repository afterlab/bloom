#include <bloom/runtime/profiler_snapshot_json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace bloom::runtime {
namespace {

class Writer final {
  public:
    explicit Writer(std::string& out) : out_(out) {}

    // Starts a value: a comma before every element or member but the first.
    void value() {
        if (pendingValue_) {
            pendingValue_ = false;
            return;
        }
        if (!first_)
            out_ += ',';
        first_ = false;
    }
    void begin(const char open) {
        value();
        out_ += open;
        first_ = true;
    }
    void end(const char close) {
        out_ += close;
        first_ = false;
    }
    void key(const std::string_view name) {
        value();
        quoted(name);
        out_ += ':';
        pendingValue_ = true;
    }
    void quoted(const std::string_view text) {
        out_ += '"';
        for (const char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            if (c == '"' || c == '\\') {
                out_ += '\\';
                out_ += raw;
            } else if (c < 0x20) {
                static constexpr std::string_view hex = "0123456789abcdef";
                out_ += "\\u00";
                out_ += hex[c >> 4U];
                out_ += hex[c & 0xFU];
            } else {
                out_ += raw;
            }
        }
        out_ += '"';
    }
    template <typename T> void number(const T number) {
        value();
        std::array<char, 32> buffer{};
        const auto [last, error] =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
        out_.append(buffer.data(), error == std::errc{} ? last : buffer.data());
    }
    void real(const float number) {
        value();
        if (!std::isfinite(number)) {
            out_ += "null";
            return;
        }
        std::array<char, 32> buffer{};
        const auto [last, error] =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<double>(number),
                          std::chars_format::general, 6);
        out_.append(buffer.data(), error == std::errc{} ? last : buffer.data());
    }
    void boolean(const bool flag) {
        value();
        out_ += flag ? "true" : "false";
    }
    void null() {
        value();
        out_ += "null";
    }

    // Members.
    void text(const std::string_view name, const std::string_view content) {
        key(name);
        value();
        quoted(content);
    }
    template <typename T> void integer(const std::string_view name, const T measured) {
        key(name);
        number(measured);
    }
    void flag(const std::string_view name, const bool measured) {
        key(name);
        boolean(measured);
    }
    template <typename T>
    void metric(const std::string_view name, const ProfilerMetric<T>& measured) {
        key(name);
        if (!measured.present)
            null();
        else if constexpr (std::is_same_v<T, bool>)
            boolean(measured.value);
        else if constexpr (std::is_floating_point_v<T>)
            real(measured.value);
        else
            number(measured.value);
    }

  private:
    std::string& out_;
    bool first_ = true;
    bool pendingValue_ = false;
};

std::string_view poolKindName(const ProfilerPoolKind kind) {
    switch (kind) {
    case ProfilerPoolKind::Scratch:
        return "scratch";
    case ProfilerPoolKind::Cache:
        return "cache";
    case ProfilerPoolKind::Live:
        return "live";
    }
    return "cache";
}

constexpr std::array<std::string_view, kProfilerStageCount> kStageNames{
    "decode", "colourConvert", "upload", "render", "transfer", "encode"};
constexpr std::array<std::string_view, kProfilerTaskStateCount> kStateNames{
    "queued", "running", "succeeded", "cancelled", "failed"};
constexpr std::array<std::string_view, kProfilerTaskOwnerKindCount> kOwnerNames{
    "application", "project", "composition", "panelRequest", "export"};
constexpr std::array<std::string_view, kProfilerTaskExecutorCount> kExecutorNames{
    "cpu", "blockingIo", "gpu"};
constexpr std::array<std::string_view, kProfilerEvictionCauseCount> kCauseNames{
    "ceiling", "memoryPressure", "admission", "purge"};

template <typename Array> std::string_view nameAt(const Array& names, const auto value) {
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : std::string_view("unknown");
}

template <std::size_t N>
void counts(Writer& json, const std::string_view name, const std::array<std::uint32_t, N>& values,
            const std::array<std::string_view, N>& names) {
    json.key(name);
    json.begin('{');
    for (std::size_t index = 0; index < N; ++index)
        json.integer(names[index], values[index]);
    json.end('}');
}

void writeMemory(Writer& json, const ProfilerMemory& memory) {
    json.key("memory");
    json.begin('{');
    json.metric("rssBytes", memory.rssBytes);
    json.metric("anonBytes", memory.anonBytes);
    json.metric("fileBytes", memory.fileBytes);
    json.metric("shmemBytes", memory.shmemBytes);
    json.metric("physicalBytes", memory.physicalBytes);
    json.metric("availableBytes", memory.availableBytes);
    json.metric("swapTotalBytes", memory.swapTotalBytes);
    json.metric("swapUsedBytes", memory.swapUsedBytes);
    json.metric("processSwappedBytes", memory.processSwappedBytes);
    json.metric("headroomBytes", memory.headroomBytes);
    json.metric("usableBytes", memory.usableBytes);
    json.metric("ledgerConfiguredBytes", memory.ledgerConfiguredBytes);
    json.metric("ledgerEffectiveBytes", memory.ledgerEffectiveBytes);
    json.metric("ledgerRetainedBytes", memory.ledgerRetainedBytes);
    json.metric("memoryPressure", memory.memoryPressure);
    json.metric("swapPressure", memory.swapPressure);
    json.integer("droppedPools", memory.droppedPools);
    json.key("pools");
    json.begin('[');
    const auto poolCount = std::min<std::size_t>(memory.poolCount, memory.pools.size());
    for (std::size_t index = 0; index < poolCount; ++index) {
        const auto& pool = memory.pools[index];
        json.begin('{');
        json.text("name", profilerNameView(pool.name));
        json.text("kind", poolKindName(pool.kind));
        json.integer("hostBytes", pool.hostBytes);
        json.integer("pendingBytes", pool.pendingBytes);
        json.integer("deviceBytes", pool.deviceBytes);
        json.metric("entries", pool.entries);
        json.metric("ceilingBytes", pool.ceilingBytes);
        json.metric("hits", pool.hits);
        json.metric("misses", pool.misses);
        json.metric("stores", pool.stores);
        json.metric("evictions", pool.evictions);
        json.metric("evictedBytes", pool.evictedBytes);
        json.metric("refusals", pool.refusals);
        json.end('}');
    }
    json.end(']');
    json.metric("stagingLiveBytes", memory.stagingLiveBytes);
    json.metric("stagingPeakBytes", memory.stagingPeakBytes);
    json.metric("admissionClaimBytes", memory.admissionClaimBytes);
    json.metric("unattributedBytes", memory.unattributedBytes);
    json.metric("idleBaselineBytes", memory.idleBaselineBytes);
    json.end('}');
}

void writeGpu(Writer& json, const ProfilerGpu& gpu) {
    json.key("gpu");
    json.begin('{');
    json.metric("liveBytes", gpu.liveBytes);
    json.metric("retiringBytes", gpu.retiringBytes);
    json.metric("refusals", gpu.refusals);
    json.metric("reclaims", gpu.reclaims);
    json.metric("heapBudgetBytes", gpu.heapBudgetBytes);
    json.metric("heapUsageBytes", gpu.heapUsageBytes);
    json.metric("uploadBytes", gpu.uploadBytes);
    json.metric("readbackBytes", gpu.readbackBytes);
    json.metric("submits", gpu.submits);
    json.metric("utilization", gpu.utilization);
    json.end('}');
}

void writeCpu(Writer& json, const ProfilerCpu& cpu) {
    json.key("cpu");
    json.begin('{');
    json.metric("processCpuNs", cpu.processCpuNs);
    json.key("executorPools");
    json.begin('[');
    const auto count = std::min<std::size_t>(cpu.executorPoolCount, cpu.executorPools.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& pool = cpu.executorPools[index];
        json.begin('{');
        json.text("name", profilerNameView(pool.name));
        json.integer("workers", pool.workers);
        json.integer("queueDepth", pool.queueDepth);
        json.metric("busyNs", pool.busyNs);
        json.end('}');
    }
    json.end(']');
    json.end('}');
}

void writeWorkload(Writer& json, const ProfilerWorkload& workload) {
    json.key("workload");
    json.begin('{');
    counts(json, "tasksByState", workload.tasksByState, kStateNames);
    counts(json, "tasksByOwner", workload.tasksByOwner, kOwnerNames);
    counts(json, "tasksByExecutor", workload.tasksByExecutor, kExecutorNames);
    json.key("tasks");
    json.begin('[');
    const auto count = std::min<std::size_t>(workload.taskCount, workload.tasks.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& task = workload.tasks[index];
        json.begin('{');
        json.text("name", profilerNameView(task.name));
        json.text("ownerKind", nameAt(kOwnerNames, task.ownerKind));
        json.integer("ownerId", task.ownerId);
        json.text("executor", nameAt(kExecutorNames, task.executor));
        json.text("state", nameAt(kStateNames, task.state));
        json.metric("progress", task.progress);
        json.integer("ageNs", task.ageNs);
        json.end('}');
    }
    json.end(']');
    json.key("stages");
    json.begin('{');
    json.integer("firstEdgeNs", kProfilerHistogramFirstEdgeNs);
    for (std::size_t stage = 0; stage < kProfilerStageCount; ++stage) {
        const auto& histogram = workload.stages[stage];
        json.key(kStageNames[stage]);
        if (!histogram.present) {
            json.null();
            continue;
        }
        json.begin('{');
        json.integer("totalNs", histogram.value.totalNs);
        std::uint64_t samples = 0;
        for (const auto value : histogram.value.counts)
            samples += value;
        json.integer("samples", samples);
        json.key("counts");
        json.begin('[');
        for (const auto value : histogram.value.counts) {
            json.number(value);
        }
        json.end(']');
        json.end('}');
    }
    json.end('}');
    json.metric("playbackFps", workload.playbackFps);
    json.metric("droppedFrames", workload.droppedFrames);
    json.metric("ramPreviewCachedFrames", workload.ramPreviewCachedFrames);
    json.metric("ramPreviewFillFps", workload.ramPreviewFillFps);
    json.metric("exportFps", workload.exportFps);
    json.end('}');
}

void writePressure(Writer& json, const ProfilerPressure& pressure) {
    json.key("pressure");
    json.begin('{');
    json.metric("admissionWaits", pressure.admissionWaits);
    json.metric("admissionRefusals", pressure.admissionRefusals);
    json.key("evictionBytes");
    json.begin('{');
    for (std::size_t cause = 0; cause < kProfilerEvictionCauseCount; ++cause)
        json.metric(kCauseNames[cause], pressure.evictionBytes[cause]);
    json.end('}');
    json.end('}');
}

void writeProcesses(Writer& json, const ProfilerProcesses& processes) {
    json.key("processes");
    json.begin('{');
    json.key("helpers");
    json.begin('[');
    const auto count = std::min<std::size_t>(processes.helperCount, processes.helpers.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& helper = processes.helpers[index];
        json.begin('{');
        json.integer("pid", helper.pid);
        json.text("name", profilerNameView(helper.name));
        json.metric("rssBytes", helper.rssBytes);
        json.metric("cpuNs", helper.cpuNs);
        json.flag("alive", helper.alive);
        json.end('}');
    }
    json.end(']');
    json.end('}');
}

} // namespace

std::string profilerSnapshotJson(const ProfilerSnapshot& snapshot) {
    std::string out;
    out.reserve(16 * 1024);
    Writer json(out);
    json.begin('{');
    json.integer("schemaVersion", snapshot.schemaVersion);
    json.integer("serial", snapshot.serial);
    json.integer("steadyTimestampNs", snapshot.steadyTimestampNs);
    json.integer("intervalNs", snapshot.intervalNs);
    json.metric("ledgerSampleAgeNs", snapshot.ledgerSampleAgeNs);
    json.metric("gpuBudgetQueryAgeNs", snapshot.gpuBudgetQueryAgeNs);
    writeMemory(json, snapshot.memory);
    writeGpu(json, snapshot.gpu);
    writeCpu(json, snapshot.cpu);
    writeWorkload(json, snapshot.workload);
    writePressure(json, snapshot.pressure);
    writeProcesses(json, snapshot.processes);
    json.end('}');
    return out;
}

} // namespace bloom::runtime
