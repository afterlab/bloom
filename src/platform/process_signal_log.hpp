#pragma once
#include <cstdint>
#include <string_view>

namespace bloom::platform::detail {
// The environment variable that turns on the supervisors' signal log (local-validation.md).
inline constexpr const char* kProcessSignalLogVariable = "BLOOM_PROCESS_SIGNAL_LOG";
// When BLOOM_PROCESS_SIGNAL_LOG names a file, every signal or termination a process supervisor
// sends is appended to it as one line before it is sent: the time, the sending process, the
// target (a process or a process group), the signal, the program the supervisor started there,
// the name the system reports for the target at that moment (Linux and macOS) and why. A signal
// that reached a process the supervisor did not start shows as a mismatch of the two names, and a
// process killed with no line naming it was not killed by a supervisor. Unset, this does nothing.
// Never throws; a log that cannot be written is skipped.
void recordProcessSignal(std::int64_t target, bool group, std::string_view signal,
                         std::string_view executable, std::string_view reason) noexcept;
} // namespace bloom::platform::detail
