#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace matchcore::http {

struct Request {
    std::string method;
    std::string path;    // without query string
    std::string query;   // raw, without leading '?'
    std::string body;
    std::map<std::string, std::string> headers;   // keys lower-cased
    std::string peer_ip;                          // TCP peer address

    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? "" : it->second;
    }
};

struct Response {
    int         status        = 200;
    std::string body;
    std::string content_type  = "application/json";
    std::string cache_control = "no-store";
};

using Handler = std::function<Response(const Request&)>;

// Minimal blocking HTTP/1.1 server over POSIX sockets.
//
// Deliberately single-threaded: one connection is read, handled and closed
// before the next is accepted (Connection: close, no keep-alive). That makes
// the server thread the *only* producer into the MatchingEngine, so API
// handlers can reason about "trades generated since my request" exactly.
// Throughput is irrelevant here — the engine is the performance-critical part,
// and it is benchmarked separately without any network layer.
class Server {
public:
    Server(std::string host, uint16_t port);
    ~Server();
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    // Serves until `stop` becomes true (polled every ~200 ms).
    void serve(const Handler& handler, const std::atomic<bool>& stop);

private:
    void handle_connection(int fd, const std::string& peer_ip, const Handler& handler);
    int  listen_fd_ = -1;
};

// Parses `key` out of a raw query string ("a=1&b=2"). No percent-decoding:
// the API only takes numeric query parameters.
std::string query_param(const std::string& query, const std::string& key);

} // namespace matchcore::http
