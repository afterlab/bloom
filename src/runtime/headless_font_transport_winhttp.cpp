// The headless font transport on Windows, over WinHTTP (part of the operating system). One blocking
// GET on the calling thread, secure transport only, with the same limits as the other platforms.

#include <bloom/runtime/headless_font_transport.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bloom::runtime::fonts {

namespace {

// Each stage of a transfer (name lookup, connection, send, receive) may take this long.
constexpr int kStageMilliseconds = 20'000;
constexpr DWORD kMaximumRedirects = 4;

// Closes a WinHTTP handle when it goes out of scope.
class Handle final {
  public:
    explicit Handle(const HINTERNET handle) noexcept : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (handle_ != nullptr)
            ::WinHttpCloseHandle(handle_);
    }
    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  private:
    HINTERNET handle_ = nullptr;
};

[[nodiscard]] std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

[[nodiscard]] bool looksOffline(const DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_TIMEOUT:
        return true;
    default:
        return false;
    }
}

class WinHttpFontTransport final : public FontTransport {
  public:
    [[nodiscard]] FontHttpResult
    get(const std::string& url, const std::uint64_t maximumBytes,
        const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t, std::uint64_t)>& progress) override {
        FontHttpResult result;
        const auto isCancelled = [&] { return cancelled && cancelled(); };
        const auto cancel = [&] {
            result.status = FontHttpStatus::Cancelled;
            result.body.clear();
            return result;
        };
        const auto fail = [&](const char* what) {
            const DWORD error = ::GetLastError();
            result.status = looksOffline(error) ? FontHttpStatus::Offline : FontHttpStatus::Failed;
            result.error = error == ERROR_WINHTTP_SECURE_FAILURE
                               ? "the secure connection could not be made"
                               : std::string(what) + " (code " + std::to_string(error) + ")";
            result.body.clear();
            return result;
        };
        if (isCancelled())
            return cancel();

        const std::wstring wide = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (wide.empty() ||
            ::WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE ||
            parts.nScheme != INTERNET_SCHEME_HTTPS || parts.lpszHostName == nullptr) {
            result.status = FontHttpStatus::Failed;
            result.error = "the address is not a secure web address";
            return result;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path;
        if (parts.lpszUrlPath != nullptr)
            path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.lpszExtraInfo != nullptr)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        const Handle session(::WinHttpOpen(L"Bloom", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session)
            return fail("the HTTP client could not start");
        ::WinHttpSetTimeouts(session.get(), kStageMilliseconds, kStageMilliseconds,
                             kStageMilliseconds, kStageMilliseconds);
        const Handle connection(::WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
        if (!connection)
            return fail("the connection could not be opened");
        const Handle request(::WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE));
        if (!request)
            return fail("the request could not be made");
        // Secure transport only, including after a redirect.
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        ::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                           sizeof(redirectPolicy));
        DWORD redirects = kMaximumRedirects;
        ::WinHttpSetOption(request.get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects,
                           sizeof(redirects));

        if (::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE)
            return fail("the request could not be sent");
        if (isCancelled())
            return cancel();
        if (::WinHttpReceiveResponse(request.get(), nullptr) == FALSE)
            return fail("no answer was received");

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (::WinHttpQueryHeaders(request.get(),
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                  WINHTTP_NO_HEADER_INDEX) == FALSE)
            return fail("the answer could not be read");
        result.httpStatus = static_cast<int>(status);
        if (status < 200 || status >= 300) {
            result.status = FontHttpStatus::HttpError;
            result.error = "HTTP status " + std::to_string(status);
            return result;
        }
        // Absent for a chunked answer; the total is then unknown.
        DWORD declared = 0;
        DWORD declaredSize = sizeof(declared);
        if (::WinHttpQueryHeaders(request.get(),
                                  WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &declared, &declaredSize,
                                  WINHTTP_NO_HEADER_INDEX) == FALSE)
            declared = 0;

        std::array<std::uint8_t, 64 * 1024> block{};
        for (;;) {
            if (isCancelled())
                return cancel();
            DWORD available = 0;
            if (::WinHttpQueryDataAvailable(request.get(), &available) == FALSE)
                return fail("the transfer failed");
            if (available == 0)
                break;
            DWORD read = 0;
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(block.size()));
            if (::WinHttpReadData(request.get(), block.data(), wanted, &read) == FALSE)
                return fail("the transfer failed");
            if (read == 0)
                break;
            if (result.body.size() + read > maximumBytes) {
                result.status = FontHttpStatus::TooLarge;
                result.body.clear();
                return result;
            }
            result.body.insert(result.body.end(), block.begin(), block.begin() + read);
            if (progress)
                progress(result.body.size(), declared);
        }
        result.status = FontHttpStatus::Ok;
        return result;
    }
};

} // namespace

std::shared_ptr<FontTransport> makeHeadlessFontTransport() {
    return std::make_shared<WinHttpFontTransport>();
}

} // namespace bloom::runtime::fonts
