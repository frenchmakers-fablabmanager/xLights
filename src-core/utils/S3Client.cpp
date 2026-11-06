/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "S3Client.h"

#include "Sha256.h"
#include "xLightsVersion.h"

#include <curl/curl.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <thread>
#include <vector>

namespace {

const std::string EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct ReadState {
    const char* data = nullptr;
    size_t len = 0;
    size_t pos = 0;
};

size_t ReadCb(char* ptr, size_t size, size_t nmemb, void* userp) {
    auto* rs = static_cast<ReadState*>(userp);
    size_t n = std::min(size * nmemb, rs->len - rs->pos);
    if (n > 0) {
        memcpy(ptr, rs->data + rs->pos, n);
        rs->pos += n;
    }
    return n;
}

int SeekCb(void* userp, curl_off_t offset, int origin) {
    auto* rs = static_cast<ReadState*>(userp);
    if (origin != SEEK_SET || offset < 0 || (size_t)offset > rs->len) {
        return CURL_SEEKFUNC_CANTSEEK;
    }
    rs->pos = (size_t)offset;
    return CURL_SEEKFUNC_OK;
}

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t HeaderCb(char* ptr, size_t size, size_t nmemb, void* userp) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userp);
    std::string line(ptr, size * nmemb);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::string value = line.substr(colon + 1);
        auto b = value.find_first_not_of(" \t");
        auto e = value.find_last_not_of(" \t\r\n");
        (*headers)[name] = (b == std::string::npos) ? "" : value.substr(b, e - b + 1);
    }
    return size * nmemb;
}

struct ProgressState {
    const std::function<bool(uint64_t)>* cb = nullptr;
    bool aborted = false;
};

int XferCb(void* userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t ulnow) {
    auto* ps = static_cast<ProgressState*>(userp);
    if (ps->cb && *ps->cb && !(*ps->cb)((uint64_t)ulnow)) {
        ps->aborted = true;
        return 1;
    }
    return 0;
}

std::string TrimSlashes(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool IsRetryable(long status) {
    return status == 0 || status == 408 || status == 429 || status >= 500;
}

std::string PathUtf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

} // namespace

std::string S3Config::R2Endpoint(const std::string& accountId) {
    return "https://" + accountId + ".r2.cloudflarestorage.com";
}

S3Client::S3Client(S3Config config) :
    _cfg(std::move(config)) {
    _cfg.endpoint = TrimSlashes(_cfg.endpoint);
    if (_cfg.region.empty()) {
        _cfg.region = "auto";
    }
}

std::string S3Client::UriEncode(const std::string& s, bool keepSlash) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string S3Client::ObjectUrl(const std::string& key) const {
    std::string encodedKey = UriEncode(key, true);
    if (_cfg.pathStyle) {
        std::string url = _cfg.endpoint + "/" + UriEncode(_cfg.bucket, false);
        if (!key.empty()) {
            url += "/" + encodedKey;
        }
        return url;
    }
    auto schemeEnd = _cfg.endpoint.find("://");
    std::string scheme = schemeEnd == std::string::npos ? "https://" : _cfg.endpoint.substr(0, schemeEnd + 3);
    std::string host = schemeEnd == std::string::npos ? _cfg.endpoint : _cfg.endpoint.substr(schemeEnd + 3);
    return scheme + _cfg.bucket + "." + host + "/" + encodedKey;
}

S3Client::Response S3Client::Request(const std::string& method, const std::string& key, const std::string& query,
                                     const char* body, size_t bodyLen, const std::string& contentType,
                                     const std::function<bool(uint64_t)>& uploadProgress) const {
    static const std::string USERAGENT = "xLights-" + xlights_version_string;
    Response r;
    std::string url = ObjectUrl(key);
    if (!query.empty()) {
        url += "?" + query;
    }

    CURL* c = curl_easy_init();
    if (c == nullptr) {
        r.curlError = "curl_easy_init failed";
        return r;
    }
    char errbuf[CURL_ERROR_SIZE] = { 0 };
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, USERAGENT.c_str());
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    // Fail a stalled transfer instead of a fixed total timeout, which a slow
    // uplink could exceed on a large part.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);

    std::string sigv4 = "aws:amz:" + _cfg.region + ":s3";
    curl_easy_setopt(c, CURLOPT_AWS_SIGV4, sigv4.c_str());
    curl_easy_setopt(c, CURLOPT_USERNAME, _cfg.accessKeyId.c_str());
    curl_easy_setopt(c, CURLOPT_PASSWORD, _cfg.secretAccessKey.c_str());

    if (_cfg.verifySSL) {
        curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    } else {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    struct curl_slist* hdrs = nullptr;
    std::string shaHdr = "x-amz-content-sha256: " + (bodyLen ? Sha256::HexOf(body, bodyLen) : EMPTY_SHA256);
    hdrs = curl_slist_append(hdrs, shaHdr.c_str());
    hdrs = curl_slist_append(hdrs, "Expect:");
    std::string ctHdr;
    if (!contentType.empty()) {
        ctHdr = "Content-Type: " + contentType;
        hdrs = curl_slist_append(hdrs, ctHdr.c_str());
    }

    ReadState rs{ body, bodyLen, 0 };
    if (method == "PUT") {
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(c, CURLOPT_READFUNCTION, ReadCb);
        curl_easy_setopt(c, CURLOPT_READDATA, &rs);
        curl_easy_setopt(c, CURLOPT_SEEKFUNCTION, SeekCb);
        curl_easy_setopt(c, CURLOPT_SEEKDATA, &rs);
        curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)bodyLen);
    } else if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, bodyLen ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bodyLen);
    } else if (method == "HEAD") {
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, HeaderCb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);

    ProgressState ps{ &uploadProgress, false };
    if (uploadProgress) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, XferCb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ps);
    }

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    r.aborted = ps.aborted;
    if (rc != CURLE_OK) {
        r.curlError = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (!r.ok() && !r.aborted) {
        spdlog::warn("S3: {} {} -> HTTP {} {}", method, url, r.status, r.curlError);
    }
    return r;
}

std::string S3Client::Describe(const Response& r) {
    if (!r.curlError.empty()) {
        return "Connection error: " + r.curlError;
    }
    pugi::xml_document doc;
    if (!r.body.empty() && doc.load_buffer(r.body.data(), r.body.size())) {
        auto err = doc.child("Error");
        if (err) {
            std::string code = err.child_value("Code");
            std::string msg = err.child_value("Message");
            return "HTTP " + std::to_string(r.status) + " " + code + (msg.empty() ? "" : ": " + msg);
        }
    }
    switch (r.status) {
    case 403:
        return "HTTP 403: access denied (check the access key, secret key and bucket permissions)";
    case 404:
        return "HTTP 404: bucket not found";
    default:
        return "HTTP " + std::to_string(r.status);
    }
}

bool S3Client::PutObject(const std::string& key, const std::string& data, const std::string& contentType, std::string& error) {
    auto r = Request("PUT", key, "", data.data(), data.size(), contentType);
    if (!r.ok()) {
        error = Describe(r);
        return false;
    }
    return true;
}

bool S3Client::DeleteObject(const std::string& key, std::string& error) {
    auto r = Request("DELETE", key, "", nullptr, 0, "");
    if (!r.ok()) {
        error = Describe(r);
        return false;
    }
    return true;
}

bool S3Client::TestConnection(const std::string& keyPrefix, std::string& error) {
    if (_cfg.endpoint.empty() || _cfg.bucket.empty() || _cfg.accessKeyId.empty() || _cfg.secretAccessKey.empty()) {
        error = "Endpoint, bucket, access key and secret key are all required";
        return false;
    }
    std::string prefix = TrimSlashes(keyPrefix);
    std::string key = (prefix.empty() ? "" : prefix + "/") + ".xlights-connection-test-" + std::to_string((long long)time(nullptr));
    if (!PutObject(key, "xLights connection test", "text/plain", error)) {
        return false;
    }
    std::string delError;
    if (!DeleteObject(key, delError)) {
        spdlog::warn("S3: test object {} could not be deleted: {}", key, delError);
    }
    return true;
}

bool S3Client::UploadFile(const std::string& key, const std::filesystem::path& file, const std::string& contentType,
                          const S3Progress& progress, std::string& error, std::string* sha256Hex, bool* cancelled) {
    if (cancelled) {
        *cancelled = false;
    }
    std::error_code ec;
    uint64_t size = (uint64_t)std::filesystem::file_size(file, ec);
    if (ec) {
        error = "Unable to read " + PathUtf8(file) + ": " + ec.message();
        return false;
    }
    if (size > _partSize) {
        return UploadMultipart(key, file, size, contentType, progress, error, sha256Hex, cancelled);
    }

    std::ifstream in(file, std::ios::binary);
    std::string data((size_t)size, '\0');
    if (!in.read(data.data(), (std::streamsize)size)) {
        error = "Unable to read " + PathUtf8(file);
        return false;
    }
    std::function<bool(uint64_t)> cb;
    if (progress) {
        cb = [&](uint64_t sent) { return progress(sent, size); };
    }
    Response r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = Request("PUT", key, "", data.data(), data.size(), contentType, cb);
        if (r.ok() || r.aborted || !IsRetryable(r.status)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
    }
    if (r.aborted) {
        if (cancelled) {
            *cancelled = true;
        }
        error = "Cancelled";
        return false;
    }
    if (!r.ok()) {
        error = Describe(r);
        return false;
    }
    if (sha256Hex) {
        *sha256Hex = Sha256::HexOf(data.data(), data.size());
    }
    if (progress) {
        progress(size, size);
    }
    return true;
}

bool S3Client::UploadMultipart(const std::string& key, const std::filesystem::path& file, uint64_t size,
                               const std::string& contentType, const S3Progress& progress, std::string& error,
                               std::string* sha256Hex, bool* cancelled) {
    constexpr uint64_t MAX_PARTS = 10000;
    constexpr uint64_t MIB = 1024 * 1024;
    uint64_t partSize = std::max<uint64_t>(_partSize, 5 * MIB);
    if ((size + partSize - 1) / partSize > MAX_PARTS) {
        partSize = ((size / MAX_PARTS) / MIB + 1) * MIB;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        error = "Unable to read " + PathUtf8(file);
        return false;
    }

    auto init = Request("POST", key, "uploads=", nullptr, 0, contentType);
    if (!init.ok()) {
        error = "Unable to start upload: " + Describe(init);
        return false;
    }
    pugi::xml_document doc;
    doc.load_buffer(init.body.data(), init.body.size());
    std::string uploadId = doc.child("InitiateMultipartUploadResult").child_value("UploadId");
    if (uploadId.empty()) {
        error = "Unable to start upload: no UploadId in the response";
        return false;
    }
    const std::string encodedId = UriEncode(uploadId, false);
    spdlog::info("S3: multipart upload of {} ({} bytes, {} byte parts) started", key, size, partSize);

    auto abort = [&]() {
        auto r = Request("DELETE", key, "uploadId=" + encodedId, nullptr, 0, "");
        if (!r.ok()) {
            spdlog::warn("S3: abort of multipart upload {} failed: {}", key, Describe(r));
        }
    };

    Sha256 whole;
    std::vector<char> buf((size_t)partSize);
    std::vector<std::string> etags;
    uint64_t sentBefore = 0;
    for (int part = 1; sentBefore < size; ++part) {
        size_t len = (size_t)std::min<uint64_t>(partSize, size - sentBefore);
        if (!in.read(buf.data(), (std::streamsize)len)) {
            error = "Read error on " + PathUtf8(file);
            abort();
            return false;
        }
        whole.update(buf.data(), len);

        std::function<bool(uint64_t)> cb;
        if (progress) {
            cb = [&](uint64_t sent) { return progress(sentBefore + sent, size); };
        }
        std::string query = "partNumber=" + std::to_string(part) + "&uploadId=" + encodedId;
        Response r;
        for (int attempt = 0; attempt < 4; ++attempt) {
            r = Request("PUT", key, query, buf.data(), len, "", cb);
            if (r.ok() || r.aborted || !IsRetryable(r.status)) {
                break;
            }
            spdlog::warn("S3: part {} failed ({}), retrying", part, Describe(r));
            std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
        }
        if (r.aborted) {
            if (cancelled) {
                *cancelled = true;
            }
            error = "Cancelled";
            abort();
            return false;
        }
        auto etag = r.headers.find("etag");
        if (!r.ok() || etag == r.headers.end() || etag->second.empty()) {
            error = "Upload of part " + std::to_string(part) + " failed: " + Describe(r);
            abort();
            return false;
        }
        etags.push_back(etag->second);
        sentBefore += len;
        if (progress && !progress(sentBefore, size)) {
            if (cancelled) {
                *cancelled = true;
            }
            error = "Cancelled";
            abort();
            return false;
        }
    }

    std::string xml = "<CompleteMultipartUpload>";
    for (size_t i = 0; i < etags.size(); ++i) {
        xml += "<Part><PartNumber>" + std::to_string(i + 1) + "</PartNumber><ETag>" + etags[i] + "</ETag></Part>";
    }
    xml += "</CompleteMultipartUpload>";

    auto done = Request("POST", key, "uploadId=" + encodedId, xml.data(), xml.size(), "application/xml");
    // S3 can report a failed completion inside a 200 response.
    bool bodyError = done.body.find("<Error>") != std::string::npos;
    if (!done.ok() || bodyError) {
        error = "Unable to complete upload: " + Describe(done);
        abort();
        return false;
    }
    if (sha256Hex) {
        *sha256Hex = Sha256::ToHex(whole.finish());
    }
    spdlog::info("S3: multipart upload of {} completed ({} parts)", key, etags.size());
    return true;
}
