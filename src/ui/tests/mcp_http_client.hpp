#pragma once

// A blocking HTTP/1.1 client for the in-app MCP server's Streamable HTTP endpoint, shared by the
// live-session tests. Each exchange runs on its own connection and must run off the UI thread.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>

namespace bloom::ui::test::mcp_http {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
inline void closeSocket(const Socket socket) { ::closesocket(socket); }
using Length = int;
struct Startup final {
    Startup() {
        WSADATA data{};
        ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~Startup() { ::WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket kInvalid = -1;
inline void closeSocket(const Socket socket) { ::close(socket); }
using Length = std::size_t;
struct Startup final {};
#endif

struct Response final {
    int status = 0;
    std::string sessionId;
    std::string body;
};

// One blocking HTTP/1.1 exchange on its own connection (runs off the UI thread).
inline Response post(const std::uint16_t port, const std::string& token, const std::string& session,
              const std::string& body) {
    Response response;
    const Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == kInvalid)
        return response;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        closeSocket(socket);
        return response;
    }
    std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                          "\r\nAuthorization: Bearer " + token +
                          "\r\nContent-Type: application/json\r\nAccept: application/json, "
                          "text/event-stream\r\nConnection: close\r\n";
    if (!session.empty())
        request += "Mcp-Session-Id: " + session + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    static_cast<void>(::send(socket, request.data(), static_cast<Length>(request.size()), 0));
    std::string answer;
    char buffer[4096];
    while (true) {
        const auto got = ::recv(socket, buffer, static_cast<Length>(sizeof buffer), 0);
        if (got <= 0)
            break;
        answer.append(buffer, static_cast<std::size_t>(got));
        const auto head = answer.find("\r\n\r\n");
        if (head != std::string::npos) {
            std::smatch length;
            const std::string headers = answer.substr(0, head);
            if (std::regex_search(headers, length,
                                  std::regex("Content-Length: ([0-9]+)", std::regex::icase)) &&
                answer.size() >= head + 4 + std::stoul(length[1].str()))
                break;
        }
    }
    closeSocket(socket);
    if (answer.size() > 12)
        response.status = std::stoi(answer.substr(9, 3));
    std::smatch id;
    if (std::regex_search(answer, id, std::regex("Mcp-Session-Id: ([0-9a-f]+)")))
        response.sessionId = id[1].str();
    if (const auto head = answer.find("\r\n\r\n"); head != std::string::npos)
        response.body = answer.substr(head + 4);
    return response;
}

inline std::string call(const int id, const std::string& tool, const std::string& arguments) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + arguments +
           "}}";
}

} // namespace bloom::ui::test::mcp_http
