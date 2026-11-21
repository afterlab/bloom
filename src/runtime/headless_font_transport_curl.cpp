// The headless font transport over the system's libcurl, loaded at run time. Only the handful of
// entry points and option numbers used here are declared (they are part of libcurl's stable ABI),
// so the build needs neither the library nor its headers.

#include <bloom/runtime/headless_font_transport.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bloom::runtime::fonts {

namespace {

using CurlHandle = void;
using CurlMultiHandle = void;
using CurlCode = int;
using CurlMultiCode = int;
using CurlOffset = std::int64_t;

// libcurl's CURLMsg: what curl_multi_info_read() returns for a finished transfer.
struct CurlMessage final {
    int kind;
    CurlHandle* easy;
    union {
        void* whatever;
        CurlCode result;
    } data;
};
constexpr int kMessageDone = 1;
constexpr CurlMultiCode kMultiOk = 0;

constexpr CurlCode kOk = 0;
constexpr CurlCode kCouldntResolveProxy = 5;
constexpr CurlCode kCouldntResolveHost = 6;
constexpr CurlCode kCouldntConnect = 7;
constexpr CurlCode kOperationTimedOut = 28;
constexpr CurlCode kSslConnectError = 35;
constexpr CurlCode kGotNothing = 52;
constexpr CurlCode kSendError = 55;
constexpr CurlCode kRecvError = 56;
constexpr CurlCode kAbortedByCallback = 42;
constexpr CurlCode kFailedInit = 2;

constexpr int kGlobalDefault = 3;
constexpr int kOptWriteData = 10001;
constexpr int kOptUrl = 10002;
constexpr int kOptUserAgent = 10018;
constexpr int kOptLowSpeedLimit = 19;
constexpr int kOptLowSpeedTime = 20;
constexpr int kOptFollowLocation = 52;
constexpr int kOptNoProgress = 43;
constexpr int kOptMaxRedirs = 68;
constexpr int kOptConnectTimeout = 78;
constexpr int kOptNoSignal = 99;
constexpr int kOptWriteFunction = 20011;
constexpr int kOptXferInfoFunction = 20219;
constexpr int kOptXferInfoData = 10057;
constexpr int kOptProtocolsString = 10318;
constexpr int kOptRedirProtocolsString = 10319;
constexpr int kInfoResponseCode = 0x200000 + 2;

// The transfer is driven here, not inside curl_easy_perform(): that call sleeps in libcurl's own
// wait until the next timer (the connect timeout during a TLS handshake) and only then calls the
// progress callback, so a cancellation could go unseen for 20 seconds (#750). Each wait here is
// at most this long, and the cancellation is checked after every one.
constexpr int kCancelCheckMilliseconds = 50;

// A transfer that moves fewer than this many bytes a second for this long has stalled.
constexpr long kStallBytesPerSecond = 1;
constexpr long kStallSeconds = 20;
constexpr long kConnectSeconds = 20;
constexpr long kMaximumRedirects = 4;

struct Library final {
    void* handle = nullptr;
    CurlCode (*globalInit)(long) = nullptr;
    CurlHandle* (*easyInit)() = nullptr;
    void (*easyCleanup)(CurlHandle*) = nullptr;
    CurlCode (*easySetopt)(CurlHandle*, int, ...) = nullptr;
    CurlCode (*easyGetinfo)(CurlHandle*, int, ...) = nullptr;
    CurlMultiHandle* (*multiInit)() = nullptr;
    CurlMultiCode (*multiCleanup)(CurlMultiHandle*) = nullptr;
    CurlMultiCode (*multiAddHandle)(CurlMultiHandle*, CurlHandle*) = nullptr;
    CurlMultiCode (*multiRemoveHandle)(CurlMultiHandle*, CurlHandle*) = nullptr;
    CurlMultiCode (*multiPerform)(CurlMultiHandle*, int*) = nullptr;
    CurlMultiCode (*multiWait)(CurlMultiHandle*, void*, unsigned, int, int*) = nullptr;
    CurlMessage* (*multiInfoRead)(CurlMultiHandle*, int*) = nullptr;
};

[[nodiscard]] const Library* library() {
    static const Library loaded = [] {
        Library lib;
#if defined(__APPLE__)
        // Part of the operating system (in its shared library cache, so there is no file to find).
        static constexpr const char* names[] = {"/usr/lib/libcurl.4.dylib"};
#else
        static constexpr const char* names[] = {"libcurl.so.4", "libcurl.so"};
#endif
        for (const char* name : names) {
            lib.handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (lib.handle != nullptr)
                break;
        }
        if (lib.handle == nullptr)
            return Library{};
        const auto symbol = [&](const char* name) { return ::dlsym(lib.handle, name); };
        // Function-pointer casts through dlsym are the documented way to bind these.
        lib.globalInit = reinterpret_cast<decltype(lib.globalInit)>(symbol("curl_global_init"));
        lib.easyInit = reinterpret_cast<decltype(lib.easyInit)>(symbol("curl_easy_init"));
        lib.easyCleanup = reinterpret_cast<decltype(lib.easyCleanup)>(symbol("curl_easy_cleanup"));
        lib.easySetopt = reinterpret_cast<decltype(lib.easySetopt)>(symbol("curl_easy_setopt"));
        lib.easyGetinfo = reinterpret_cast<decltype(lib.easyGetinfo)>(symbol("curl_easy_getinfo"));
        lib.multiInit = reinterpret_cast<decltype(lib.multiInit)>(symbol("curl_multi_init"));
        lib.multiCleanup =
            reinterpret_cast<decltype(lib.multiCleanup)>(symbol("curl_multi_cleanup"));
        lib.multiAddHandle =
            reinterpret_cast<decltype(lib.multiAddHandle)>(symbol("curl_multi_add_handle"));
        lib.multiRemoveHandle =
            reinterpret_cast<decltype(lib.multiRemoveHandle)>(symbol("curl_multi_remove_handle"));
        lib.multiPerform =
            reinterpret_cast<decltype(lib.multiPerform)>(symbol("curl_multi_perform"));
        lib.multiWait = reinterpret_cast<decltype(lib.multiWait)>(symbol("curl_multi_wait"));
        lib.multiInfoRead =
            reinterpret_cast<decltype(lib.multiInfoRead)>(symbol("curl_multi_info_read"));
        if (lib.globalInit == nullptr || lib.easyInit == nullptr || lib.easyCleanup == nullptr ||
            lib.easySetopt == nullptr || lib.easyGetinfo == nullptr || lib.multiInit == nullptr ||
            lib.multiCleanup == nullptr || lib.multiAddHandle == nullptr ||
            lib.multiRemoveHandle == nullptr || lib.multiPerform == nullptr ||
            lib.multiWait == nullptr || lib.multiInfoRead == nullptr ||
            lib.globalInit(kGlobalDefault) != kOk)
            return Library{};
        return lib;
    }();
    return loaded.handle != nullptr ? &loaded : nullptr;
}

struct Transfer final {
    const std::function<bool()>* cancelled = nullptr;
    const std::function<void(std::uint64_t, std::uint64_t)>* progress = nullptr;
    std::uint64_t maximumBytes = 0;
    std::vector<std::uint8_t>* body = nullptr;
    bool wasCancelled = false;
    bool tooLarge = false;
};

std::size_t onWrite(char* data, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<Transfer*>(user);
    const auto bytes = size * count;
    if (transfer.body->size() + bytes > transfer.maximumBytes) {
        transfer.tooLarge = true;
        return 0; // Stops the transfer with a write error.
    }
    transfer.body->insert(transfer.body->end(), reinterpret_cast<const std::uint8_t*>(data),
                          reinterpret_cast<const std::uint8_t*>(data) + bytes);
    return bytes;
}

int onProgress(void* user, CurlOffset total, CurlOffset received, CurlOffset, CurlOffset) {
    auto& transfer = *static_cast<Transfer*>(user);
    if (transfer.cancelled != nullptr && *transfer.cancelled && (*transfer.cancelled)()) {
        transfer.wasCancelled = true;
        return 1;
    }
    if (transfer.progress != nullptr && *transfer.progress)
        (*transfer.progress)(static_cast<std::uint64_t>(std::max<CurlOffset>(received, 0)),
                             static_cast<std::uint64_t>(std::max<CurlOffset>(total, 0)));
    return 0;
}

[[nodiscard]] bool looksOffline(const CurlCode code) {
    switch (code) {
    case kCouldntResolveProxy:
    case kCouldntResolveHost:
    case kCouldntConnect:
    case kOperationTimedOut:
    case kGotNothing:
    case kSendError:
    case kRecvError:
        return true;
    default:
        return false;
    }
}

// Runs one transfer to its end, checking for cancellation between waits of at most
// kCancelCheckMilliseconds. A cancellation ends it as aborted by the callback.
[[nodiscard]] CurlCode perform(const Library& lib, CurlHandle* easy, Transfer& transfer) {
    CurlMultiHandle* multi = lib.multiInit();
    if (multi == nullptr)
        return kFailedInit;
    if (lib.multiAddHandle(multi, easy) != kMultiOk) {
        lib.multiCleanup(multi);
        return kFailedInit;
    }
    const auto cancelled = [&] {
        if (transfer.cancelled != nullptr && *transfer.cancelled && (*transfer.cancelled)())
            transfer.wasCancelled = true;
        return transfer.wasCancelled;
    };
    CurlCode code = kFailedInit;
    bool done = false;
    while (!done) {
        int running = 0;
        if (lib.multiPerform(multi, &running) != kMultiOk)
            break;
        int queued = 0;
        while (const CurlMessage* message = lib.multiInfoRead(multi, &queued))
            if (message->kind == kMessageDone && message->easy == easy) {
                code = message->data.result;
                done = true;
            }
        if (done || running == 0)
            break;
        if (cancelled()) {
            code = kAbortedByCallback;
            break;
        }
        int ready = 0;
        if (lib.multiWait(multi, nullptr, 0, kCancelCheckMilliseconds, &ready) != kMultiOk)
            break;
        if (cancelled()) {
            code = kAbortedByCallback;
            break;
        }
    }
    lib.multiRemoveHandle(multi, easy);
    lib.multiCleanup(multi);
    return code;
}

class CurlFontTransport final : public FontTransport {
  public:
    [[nodiscard]] FontHttpResult
    get(const std::string& url, const std::uint64_t maximumBytes,
        const std::function<bool()>& cancelled,
        const std::function<void(std::uint64_t, std::uint64_t)>& progress) override {
        FontHttpResult result;
        const Library* lib = library();
        if (lib == nullptr) {
            result.status = FontHttpStatus::Failed;
            result.error = "the system HTTP client is not available";
            return result;
        }
        if (cancelled && cancelled()) {
            result.status = FontHttpStatus::Cancelled;
            return result;
        }
        CurlHandle* easy = lib->easyInit();
        if (easy == nullptr) {
            result.status = FontHttpStatus::Failed;
            result.error = "the HTTP client could not start";
            return result;
        }
        Transfer transfer;
        transfer.cancelled = &cancelled;
        transfer.progress = &progress;
        transfer.maximumBytes = maximumBytes;
        transfer.body = &result.body;
        lib->easySetopt(easy, kOptUrl, url.c_str());
        lib->easySetopt(easy, kOptUserAgent, "Bloom");
        lib->easySetopt(easy, kOptWriteFunction, &onWrite);
        lib->easySetopt(easy, kOptWriteData, &transfer);
        lib->easySetopt(easy, kOptNoProgress, 0L);
        lib->easySetopt(easy, kOptXferInfoFunction, &onProgress);
        lib->easySetopt(easy, kOptXferInfoData, &transfer);
        lib->easySetopt(easy, kOptNoSignal, 1L);
        lib->easySetopt(easy, kOptConnectTimeout, kConnectSeconds);
        lib->easySetopt(easy, kOptLowSpeedLimit, kStallBytesPerSecond);
        lib->easySetopt(easy, kOptLowSpeedTime, kStallSeconds);
        // Secure transport only, including after a redirect. An older library that does not know
        // the option leaves the URL check in the resolver (it accepts only https addresses).
        lib->easySetopt(easy, kOptProtocolsString, "https");
        lib->easySetopt(easy, kOptRedirProtocolsString, "https");
        lib->easySetopt(easy, kOptFollowLocation, 1L);
        lib->easySetopt(easy, kOptMaxRedirs, kMaximumRedirects);

        const CurlCode code = perform(*lib, easy, transfer);
        long http = 0;
        lib->easyGetinfo(easy, kInfoResponseCode, &http);
        lib->easyCleanup(easy);
        result.httpStatus = static_cast<int>(http);
        if (transfer.wasCancelled || code == kAbortedByCallback) {
            result.status = FontHttpStatus::Cancelled;
            result.body.clear();
        } else if (transfer.tooLarge) {
            result.status = FontHttpStatus::TooLarge;
            result.body.clear();
        } else if (code == kOk && http >= 200 && http < 300) {
            result.status = FontHttpStatus::Ok;
        } else if (code == kOk) {
            result.status = FontHttpStatus::HttpError;
            result.error = "HTTP status " + std::to_string(http);
            result.body.clear();
        } else {
            result.status = looksOffline(code) ? FontHttpStatus::Offline : FontHttpStatus::Failed;
            result.error = code == kSslConnectError
                               ? "the secure connection could not be made"
                               : "the transfer failed (code " + std::to_string(code) + ")";
            result.body.clear();
        }
        return result;
    }
};

} // namespace

std::shared_ptr<FontTransport> makeHeadlessFontTransport() {
    if (library() == nullptr)
        return nullptr;
    return std::make_shared<CurlFontTransport>();
}

} // namespace bloom::runtime::fonts
