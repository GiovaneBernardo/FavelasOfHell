#include "http_client.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <unordered_map>

#include <curl/curl.h>

#include "foh/log.h"

namespace foh {

namespace {
constexpr size_t kMaxConcurrent = 8;
constexpr const char* kUserAgent = "FavelasOfHell-Editor/0.1 (asset workshop)";
} // namespace

struct HttpClient::Impl {
    struct Transfer {
        Id id = 0;
        HttpRequest request;
        Callback done;
        CURL* easy = nullptr;
        curl_slist* headers = nullptr;
        HttpResult result;
        std::ofstream file;
        std::filesystem::path partPath;
        std::uint64_t received = 0;
        std::uint64_t total = 0;
        bool tooBig = false;
        bool cancelled = false;
        char errorBuffer[CURL_ERROR_SIZE] = {};
    };

    CURLM* multi = nullptr;
    Id nextId = 1;
    std::deque<std::unique_ptr<Transfer>> queued;
    std::unordered_map<CURL*, std::unique_ptr<Transfer>> active;

    static size_t onWrite(char* data, size_t size, size_t count, void* user) {
        auto* t = static_cast<Transfer*>(user);
        const size_t n = size * count;
        if (t->cancelled || t->received + n > t->request.maxBytes) {
            t->tooBig = !t->cancelled;
            return 0; // aborts the transfer
        }
        t->received += n;
        if (!t->request.toFile.empty()) {
            t->file.write(data, static_cast<std::streamsize>(n));
            return t->file ? n : 0;
        }
        t->result.body.append(data, n);
        return n;
    }

    static int onProgress(void* user, curl_off_t dltotal, curl_off_t, curl_off_t, curl_off_t) {
        auto* t = static_cast<Transfer*>(user);
        t->total = dltotal > 0 ? static_cast<std::uint64_t>(dltotal) : 0;
        return t->cancelled ? 1 : 0;
    }

    void start(std::unique_ptr<Transfer> t) {
        t->easy = curl_easy_init();
        if (!t->easy) {
            t->result.error = "curl_easy_init failed";
            finish(std::move(t));
            return;
        }
        if (!t->request.toFile.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(t->request.toFile.parent_path(), ec);
            t->partPath = t->request.toFile;
            t->partPath += ".part";
            t->file.open(t->partPath, std::ios::binary | std::ios::trunc);
            if (!t->file) {
                curl_easy_cleanup(t->easy);
                t->easy = nullptr;
                t->result.error = "cannot write " + t->partPath.string();
                finish(std::move(t));
                return;
            }
        }
        CURL* e = t->easy;
        curl_easy_setopt(e, CURLOPT_URL, t->request.url.c_str());
        curl_easy_setopt(e, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(e, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L); // stalled for 30 s: give up
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, ""); // gzip/deflate for the JSON listings
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &Impl::onWrite);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, t.get());
        curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, &Impl::onProgress);
        curl_easy_setopt(e, CURLOPT_XFERINFODATA, t.get());
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t->errorBuffer);
        for (const std::string& h : t->request.headers) t->headers = curl_slist_append(t->headers, h.c_str());
        if (t->headers) curl_easy_setopt(e, CURLOPT_HTTPHEADER, t->headers);
        curl_multi_add_handle(multi, e);
        active.emplace(e, std::move(t));
    }

    void finish(std::unique_ptr<Transfer> t) {
        if (t->file.is_open()) t->file.close();
        if (t->easy) {
            curl_multi_remove_handle(multi, t->easy);
            curl_easy_cleanup(t->easy);
            t->easy = nullptr;
        }
        if (t->headers) curl_slist_free_all(t->headers);
        t->headers = nullptr;
        std::error_code ec;
        if (!t->request.toFile.empty()) {
            if (t->result.ok) {
                std::filesystem::rename(t->partPath, t->request.toFile, ec);
                if (ec) {
                    std::filesystem::remove(t->request.toFile, ec);
                    std::filesystem::rename(t->partPath, t->request.toFile, ec);
                }
                if (ec) {
                    t->result.ok = false;
                    t->result.error = "cannot move download into place: " + ec.message();
                } else {
                    t->result.file = t->request.toFile;
                }
            } else {
                std::filesystem::remove(t->partPath, ec);
            }
        }
        if (t->cancelled) {
            t->result.ok = false;
            t->result.error = "cancelled";
        }
        if (t->done) t->done(t->result);
    }
};

HttpClient::HttpClient() : m_impl(std::make_unique<Impl>()) {
    static bool globalInit = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    if (!globalInit) FOH_ERROR("curl_global_init failed");
    m_impl->multi = curl_multi_init();
}

HttpClient::~HttpClient() {
    for (auto& [easy, t] : m_impl->active) t->cancelled = true;
    for (auto& [easy, t] : m_impl->active) {
        t->done = nullptr; // no callbacks into half-destroyed owners
        m_impl->finish(std::move(t));
    }
    m_impl->active.clear();
    if (m_impl->multi) curl_multi_cleanup(m_impl->multi);
}

HttpClient::Id HttpClient::submit(HttpRequest request, Callback onDone) {
    auto t = std::make_unique<Impl::Transfer>();
    t->id = m_impl->nextId++;
    t->request = std::move(request);
    t->done = std::move(onDone);
    const Id id = t->id;
    m_impl->queued.push_back(std::move(t));
    return id;
}

void HttpClient::cancel(Id id) {
    for (auto& t : m_impl->queued)
        if (t->id == id) t->cancelled = true;
    for (auto& [easy, t] : m_impl->active)
        if (t->id == id) t->cancelled = true;
}

bool HttpClient::progress(Id id, HttpProgress& out) const {
    for (const auto& [easy, t] : m_impl->active) {
        if (t->id == id) {
            out.received = t->received;
            out.total = t->total;
            return true;
        }
    }
    return false;
}

size_t HttpClient::activeCount() const { return m_impl->active.size() + m_impl->queued.size(); }

void HttpClient::poll() {
    Impl& s = *m_impl;
    while (!s.queued.empty() && s.active.size() < kMaxConcurrent) {
        std::unique_ptr<Impl::Transfer> t = std::move(s.queued.front());
        s.queued.pop_front();
        if (t->cancelled) {
            t->result.error = "cancelled";
            if (t->done) t->done(t->result);
            continue;
        }
        s.start(std::move(t));
    }
    int running = 0;
    curl_multi_perform(s.multi, &running);
    int pending = 0;
    while (CURLMsg* msg = curl_multi_info_read(s.multi, &pending)) {
        if (msg->msg != CURLMSG_DONE) continue;
        auto it = s.active.find(msg->easy_handle);
        if (it == s.active.end()) continue;
        std::unique_ptr<Impl::Transfer> t = std::move(it->second);
        s.active.erase(it);
        long status = 0;
        curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &status);
        t->result.status = status;
        if (msg->data.result != CURLE_OK) {
            t->result.error = t->tooBig ? "response is larger than the allowed size"
                              : t->errorBuffer[0] ? t->errorBuffer
                                                  : curl_easy_strerror(msg->data.result);
        } else if (status < 200 || status >= 300) {
            t->result.error = "HTTP " + std::to_string(status);
        } else {
            t->result.ok = true;
        }
        s.finish(std::move(t));
    }
}

} // namespace foh
