#pragma once
// Serves the built frontend (frontend/dist) so a deployment is a single
// process on a single origin: the UI and /api come from the same server.

#include "HttpServer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace matchcore::http {

class StaticFiles {
public:
    explicit StaticFiles(const std::filesystem::path& root)
        : root_(std::filesystem::canonical(root)) {}

    Response serve(const Request& req) const {
        if (req.method != "GET" && req.method != "HEAD")
            return {405, R"({"error":"use GET"})"};

        const std::string& p = req.path;
        // Reject traversal outright; the canonical-prefix check below is the backstop.
        if (p.empty() || p[0] != '/' || p.find("..") != std::string::npos ||
            p.find('\\') != std::string::npos || p.find('\0') != std::string::npos)
            return {404, R"({"error":"not found"})"};

        std::string rel = p == "/" ? "index.html" : p.substr(1);
        if (auto r = load(rel)) return *r;

        // Unknown extensionless path → SPA route: hand back the app shell.
        if (std::filesystem::path(rel).extension().empty())
            if (auto r = load("index.html")) return *r;

        return {404, R"({"error":"not found"})"};
    }

private:
    std::optional<Response> load(const std::string& rel) const {
        std::error_code ec;
        auto full = std::filesystem::weakly_canonical(root_ / rel, ec);
        if (ec || !is_within_root(full) || !std::filesystem::is_regular_file(full, ec))
            return std::nullopt;

        std::ifstream in(full, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream buf;
        buf << in.rdbuf();

        Response r{200, buf.str(), mime(full.extension().string())};
        // Vite emits content-hashed file names under /assets, so they never change.
        r.cache_control = rel.rfind("assets/", 0) == 0 ? "public, max-age=31536000, immutable"
                                                       : "no-cache";
        return r;
    }

    bool is_within_root(const std::filesystem::path& p) const {
        auto [root_end, _] = std::mismatch(root_.begin(), root_.end(), p.begin(), p.end());
        return root_end == root_.end();
    }

    static std::string mime(const std::string& ext) {
        if (ext == ".html") return "text/html; charset=utf-8";
        if (ext == ".js")   return "text/javascript; charset=utf-8";
        if (ext == ".css")  return "text/css; charset=utf-8";
        if (ext == ".json") return "application/json";
        if (ext == ".svg")  return "image/svg+xml";
        if (ext == ".png")  return "image/png";
        if (ext == ".ico")  return "image/x-icon";
        if (ext == ".woff2") return "font/woff2";
        return "application/octet-stream";
    }

    std::filesystem::path root_;
};

} // namespace matchcore::http
