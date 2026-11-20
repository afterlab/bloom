#pragma once

// Process-wide facts a crash report needs (issue #736; docs/architecture/platform-support.md,
// "Crash Reports"). They live in the core module because the code that knows them spans the
// programs, the UI and the GPU backend, and only the crash reporter (src/crash) keeps them.
//
// Until a program installs a sink, every call does nothing. The sink is installed once, early in
// main, before other threads start, and stays for the life of the process. A call never throws;
// it runs on the caller's thread and is meant for ordinary code, never for a signal handler.

#include <string_view>

namespace bloom::core {

class DiagnosticRecordSink {
  public:
    DiagnosticRecordSink() = default;
    DiagnosticRecordSink(const DiagnosticRecordSink&) = delete;
    DiagnosticRecordSink& operator=(const DiagnosticRecordSink&) = delete;

    // One line the program wrote to its log (standard error, Qt's messages), without its line end.
    virtual void logLine(std::string_view line) noexcept = 0;
    // The GPU device the program created most recently, and its driver, in one line.
    virtual void gpuDevice(std::string_view description) noexcept = 0;

  protected:
    ~DiagnosticRecordSink() = default;
};

// Installs the sink; a later call replaces it. The sink must outlive every later record call.
void setDiagnosticRecordSink(DiagnosticRecordSink* sink) noexcept;

void recordLogLine(std::string_view line) noexcept;
void recordGpuDevice(std::string_view description) noexcept;

} // namespace bloom::core
