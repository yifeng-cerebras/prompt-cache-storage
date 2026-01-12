#include "s3_storage.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <curl/curl.h>

namespace prompt_cache_poc {

namespace {

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t total = size * nmemb;
    out->insert(out->end(), ptr, ptr + total);
    return total;
}

bool IsSuccessStatus(long code, const std::initializer_list<long>& accepted) {
    return std::find(accepted.begin(), accepted.end(), code) != accepted.end();
}

std::string TrimTrailingSlash(const std::string& s) {
    if (!s.empty() && s.back() == '/') {
        return s.substr(0, s.size() - 1);
    }
    return s;
}

} // namespace

S3Storage::S3Storage(Config cfg) : cfg_(std::move(cfg)) {
    static std::once_flag init_flag;
    std::call_once(init_flag, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

S3Storage::~S3Storage() = default;

bool S3Storage::CreateBucket() {
    long code = 0;
    std::string url = BuildBucketUrl();
    return PerformRequest(url, "PUT", nullptr, nullptr, "", &code) && IsSuccessStatus(code, {200, 204});
}

bool S3Storage::Put(const std::string& obj_id, const std::vector<uint8_t>& data) {
    long code = 0;
    std::string url = BuildObjectUrl(obj_id);
    return PerformRequest(url, "PUT", &data, nullptr, "", &code) && (code >= 200 && code < 300);
}

bool S3Storage::GetRange(const std::string& obj_id, int max_bytes, std::vector<uint8_t>& out) const {
    long code = 0;
    std::string url = BuildObjectUrl(obj_id);
    std::string range;
    if (max_bytes > 0) {
        range = "bytes=0-" + std::to_string(max_bytes - 1);
    }
    if (!PerformRequest(url, "GET", nullptr, &out, range, &code)) {
        return false;
    }
    return IsSuccessStatus(code, {200, 206});
}

bool S3Storage::Delete(const std::string& obj_id) {
    long code = 0;
    std::string url = BuildObjectUrl(obj_id);
    return PerformRequest(url, "DELETE", nullptr, nullptr, "", &code) && (code >= 200 && code < 300);
}

size_t S3Storage::Size() const {
    return 0;
}

std::string S3Storage::BuildBucketUrl() const {
    return TrimTrailingSlash(cfg_.endpoint) + "/" + cfg_.bucket;
}

std::string S3Storage::BuildObjectUrl(const std::string& obj_id) const {
    return TrimTrailingSlash(cfg_.endpoint) + "/" + cfg_.bucket + "/" + obj_id;
}

bool S3Storage::PerformRequest(const std::string& url,
                               const std::string& method,
                               const std::vector<uint8_t>* body,
                               std::vector<uint8_t>* out,
                               const std::string& range_header,
                               long* http_code) const {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    if (!range_header.empty()) {
        headers = curl_slist_append(headers, ("Range: " + range_header).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, cfg_.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (!cfg_.verify_tls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (body && (method == "PUT" || method == "POST")) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, reinterpret_cast<const char*>(body->data()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }

    if (out) {
        out->clear();
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (http_code) {
        *http_code = code;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return true;
}

} // namespace prompt_cache_poc
