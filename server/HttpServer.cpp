#include "HttpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>

namespace matchcore::http {

namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes   = 64 * 1024;

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        default:  return "Internal Server Error";
    }
}

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return;   // client went away or timed out; nothing to recover
        sent += static_cast<size_t>(n);
    }
}

void send_response(int fd, const Response& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    out += "Content-Type: " + r.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Cache-Control: " + r.cache_control + "\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "Connection: close\r\n\r\n";
    out += r.body;
    send_all(fd, out);
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

Server::Server(std::string host, uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket() failed");

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("invalid IPv4 host: " + host);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0)
        throw std::runtime_error("bind() failed on " + host + ":" + std::to_string(port) +
                                 " (port in use?)");
    if (::listen(listen_fd_, 64) < 0) throw std::runtime_error("listen() failed");
}

Server::~Server() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

void Server::serve(const Handler& handler, const std::atomic<bool>& stop) {
    while (!stop.load()) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;   // timeout or EINTR → re-check stop

        sockaddr_in peer{};
        socklen_t   peer_len = sizeof peer;
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) continue;
        char ip[INET_ADDRSTRLEN] = "";
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

        // A slow or idle client must not wedge the single server thread.
        timeval tv{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        handle_connection(fd, ip, handler);
        ::close(fd);
    }
}

void Server::handle_connection(int fd, const std::string& peer_ip, const Handler& handler) {
    std::string buf;
    char chunk[4096];
    size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) return;
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
        if (header_end == std::string::npos && buf.size() > kMaxHeaderBytes) {
            send_response(fd, {413, R"({"error":"headers too large"})"});
            return;
        }
    }

    Request req;
    req.peer_ip = peer_ip;
    std::string_view head(buf.data(), header_end);

    // Request line: METHOD SP TARGET SP VERSION
    size_t line_end = head.find("\r\n");
    std::string_view line = head.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 == std::string_view::npos ? sp1 : sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
        send_response(fd, {400, R"({"error":"malformed request line"})"});
        return;
    }
    req.method = std::string(line.substr(0, sp1));
    std::string target(line.substr(sp1 + 1, sp2 - sp1 - 1));
    size_t q = target.find('?');
    req.path  = target.substr(0, q);
    req.query = (q == std::string::npos) ? "" : target.substr(q + 1);

    // Chunked request bodies are not supported; Content-Length is required.
    size_t content_length = 0;
    size_t pos = (line_end == std::string_view::npos) ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos) eol = head.size();
        std::string_view h = head.substr(pos, eol - pos);
        size_t colon = h.find(':');
        if (colon != std::string_view::npos) {
            std::string name = lower(h.substr(0, colon));
            std::string_view value = h.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            req.headers[name] = std::string(value);
            if (name == "content-length") {
                try {
                    content_length = std::stoul(std::string(value));
                } catch (...) {
                    send_response(fd, {400, R"({"error":"bad Content-Length"})"});
                    return;
                }
            }
        }
        pos = eol + 2;
    }
    if (content_length > kMaxBodyBytes) {
        send_response(fd, {413, R"({"error":"body too large"})"});
        return;
    }

    req.body = buf.substr(header_end + 4);
    while (req.body.size() < content_length) {
        ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) return;
        req.body.append(chunk, static_cast<size_t>(n));
    }
    req.body.resize(content_length);

    Response resp;
    try {
        resp = handler(req);
    } catch (const std::exception&) {
        resp = {500, R"({"error":"internal error"})"};
    }
    send_response(fd, resp);
}

std::string query_param(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string_view kv(query.data() + pos, amp - pos);
        size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key)
            return eq == std::string_view::npos ? "" : std::string(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

} // namespace matchcore::http
