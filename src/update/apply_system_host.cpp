#include <bloom/update/apply.hpp>

#include <map>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <cstdio>
#endif
#endif

namespace bloom::update {
namespace {

#if defined(_WIN32)

[[nodiscard]] std::wstring quoted(const std::wstring& argument) {
    // The rules CommandLineToArgvW reverses: backslashes before a quote are doubled.
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
        }
        if (character != L'\\') {
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

[[nodiscard]] std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

class SystemApplyHost final : public ApplyHost {
  public:
    ~SystemApplyHost() override {
        for (const auto& entry : handles_) {
            CloseHandle(entry.second);
        }
    }

    [[nodiscard]] bool processRunning(const std::uint64_t processId) override {
        auto found = handles_.find(processId);
        if (found == handles_.end()) {
            const HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE,
                                              static_cast<DWORD>(processId));
            if (handle == nullptr) {
                // No such process, or one this user may not even wait for: not Bloom.
                return false;
            }
            found = handles_.emplace(processId, handle).first;
        }
        return WaitForSingleObject(found->second, 0) == WAIT_TIMEOUT;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    launch(const std::filesystem::path& program,
           const std::vector<std::string>& arguments) override {
        std::wstring command = quoted(program.wstring());
        for (const auto& argument : arguments) {
            command += L' ';
            command += quoted(wide(argument));
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const auto directory = program.parent_path().wstring();
        if (CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                           directory.empty() ? nullptr : directory.c_str(), &startup,
                           &process) == 0) {
            return std::nullopt;
        }
        CloseHandle(process.hThread);
        handles_.emplace(process.dwProcessId, process.hProcess);
        return process.dwProcessId;
    }

    void terminate(const std::uint64_t processId) override {
        if (processRunning(processId)) {
            TerminateProcess(handles_.at(processId), 1);
            WaitForSingleObject(handles_.at(processId), 5000);
        }
    }

    void wait(const std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
    }

    [[nodiscard]] std::error_code rename(const std::filesystem::path& from,
                                         const std::filesystem::path& to) override {
        std::error_code error;
        std::filesystem::rename(from, to, error);
        return error;
    }

    [[nodiscard]] std::error_code exchange(const std::filesystem::path& /*first*/,
                                           const std::filesystem::path& /*second*/) override {
        return std::make_error_code(std::errc::function_not_supported);
    }

    [[nodiscard]] std::string identity(const std::filesystem::path& /*path*/) override {
        return {};
    }

  private:
    std::map<std::uint64_t, HANDLE> handles_;
};

#else

class SystemApplyHost final : public ApplyHost {
  public:
    [[nodiscard]] bool processRunning(const std::uint64_t processId) override {
        const auto pid = static_cast<pid_t>(processId);
        if (pid <= 0) {
            return false;
        }
        // A program this helper started is its child and has to be collected; one it did not
        // start is only asked about.
        int status = 0;
        const pid_t collected = ::waitpid(pid, &status, WNOHANG);
        if (collected == pid) {
            return false;
        }
        if (::kill(pid, 0) != 0 && errno == ESRCH) {
            return false;
        }
#if defined(__linux__)
        // A process that has exited and waits for its parent to collect it is over.
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string content;
        std::getline(stat, content);
        if (const auto close = content.rfind(')');
            close != std::string::npos && close + 2 < content.size() && content[close + 2] == 'Z') {
            return false;
        }
#endif
        return true;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    launch(const std::filesystem::path& program,
           const std::vector<std::string>& arguments) override {
        std::vector<std::string> owned;
        owned.push_back(program.string());
        owned.insert(owned.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(owned.size() + 1);
        for (auto& argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const pid_t child = ::fork();
        if (child < 0) {
            return std::nullopt;
        }
        if (child == 0) {
            // A session of its own, so it does not end with the helper, and no terminal.
            ::setsid();
            const int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) {
                ::dup2(null, STDIN_FILENO);
                ::dup2(null, STDOUT_FILENO);
                ::dup2(null, STDERR_FILENO);
                if (null > STDERR_FILENO) {
                    ::close(null);
                }
            }
            ::execv(argv.front(), argv.data());
            ::_exit(127);
        }
        return static_cast<std::uint64_t>(child);
    }

    void terminate(const std::uint64_t processId) override {
        const auto pid = static_cast<pid_t>(processId);
        if (pid <= 0 || !processRunning(processId)) {
            return;
        }
        ::kill(pid, SIGTERM);
        for (int attempt = 0; attempt < 25 && processRunning(processId); ++attempt) {
            wait(std::chrono::milliseconds(200));
        }
        if (processRunning(processId)) {
            ::kill(pid, SIGKILL);
            for (int attempt = 0; attempt < 25 && processRunning(processId); ++attempt) {
                wait(std::chrono::milliseconds(200));
            }
        }
    }

    void wait(const std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
    }

    [[nodiscard]] std::error_code rename(const std::filesystem::path& from,
                                         const std::filesystem::path& to) override {
        std::error_code error;
        std::filesystem::rename(from, to, error);
        return error;
    }

    [[nodiscard]] std::error_code exchange(const std::filesystem::path& first,
                                           const std::filesystem::path& second) override {
#if defined(__linux__)
        if (::syscall(SYS_renameat2, AT_FDCWD, first.c_str(), AT_FDCWD, second.c_str(),
                      kRenameExchange) == 0) {
            return {};
        }
        return {errno, std::generic_category()};
#elif defined(__APPLE__)
        if (::renamex_np(first.c_str(), second.c_str(), RENAME_SWAP) == 0) {
            return {};
        }
        return {errno, std::generic_category()};
#else
        (void)first;
        (void)second;
        return std::make_error_code(std::errc::function_not_supported);
#endif
    }

    [[nodiscard]] std::string identity(const std::filesystem::path& path) override {
        struct stat status {};
        if (::lstat(path.c_str(), &status) != 0) {
            return {};
        }
        return std::to_string(static_cast<unsigned long long>(status.st_dev)) + ':' +
               std::to_string(static_cast<unsigned long long>(status.st_ino));
    }

  private:
#if defined(__linux__)
    // RENAME_EXCHANGE of renameat2(2), spelled here because older C library headers lack it.
    static constexpr unsigned int kRenameExchange = 2;
#endif
};

#endif

} // namespace

std::unique_ptr<ApplyHost> makeSystemApplyHost() { return std::make_unique<SystemApplyHost>(); }

} // namespace bloom::update
