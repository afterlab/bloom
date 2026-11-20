#include <bloom/core/diagnostic_record.hpp>

#include <atomic>

namespace bloom::core {
namespace {
std::atomic<DiagnosticRecordSink*> installedSink{nullptr};
} // namespace

void setDiagnosticRecordSink(DiagnosticRecordSink* const sink) noexcept {
    installedSink.store(sink, std::memory_order_release);
}

void recordLogLine(const std::string_view line) noexcept {
    if (auto* const sink = installedSink.load(std::memory_order_acquire))
        sink->logLine(line);
}

void recordGpuDevice(const std::string_view description) noexcept {
    if (auto* const sink = installedSink.load(std::memory_order_acquire))
        sink->gpuDevice(description);
}

} // namespace bloom::core
