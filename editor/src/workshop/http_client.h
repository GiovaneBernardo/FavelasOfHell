#pragma once

// Non-blocking HTTPS client for the workshop (libcurl's multi interface, driven from the main
// thread: poll() once per frame, no threads and no blocking waits).
//
// Safety: only https is accepted, including after redirects; response sizes are capped; downloads
// stream into "<file>.part" and are renamed on success, so a cancelled or broken transfer never
// leaves a half-written file under its final name.
//
// Thread-safety: main thread only. Callbacks run inside poll().

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace foh {

struct HttpRequest {
    std::string url;
    std::vector<std::string> headers;     // "Name: value"
    std::filesystem::path toFile;         // empty: keep the body in memory
    std::uint64_t maxBytes = 64ull << 20; // hard cap; downloads to a file should raise it
};

struct HttpResult {
    bool ok = false; // transfer completed with a 2xx status
    long status = 0;
    std::string error;
    std::string body;               // in-memory responses
    std::filesystem::path file;     // file responses
};

struct HttpProgress {
    std::uint64_t received = 0;
    std::uint64_t total = 0; // 0 = unknown
};

class HttpClient {
public:
    using Id = std::uint32_t;
    using Callback = std::function<void(HttpResult&)>;

    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Queues a request. The callback is invoked exactly once (also on failure or cancel).
    Id submit(HttpRequest request, Callback onDone);
    void cancel(Id id);
    // Drives all transfers; call every frame.
    void poll();

    bool progress(Id id, HttpProgress& out) const;
    size_t activeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace foh
