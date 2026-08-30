#include "http.h"
#include <3ds.h>
#include <cstring>

// Hard cap on response body to protect against runaway downloads
static constexpr u32 MAX_BODY = 512 * 1024;
static constexpr unsigned MAX_REDIRECTS = 5;
// Azahar currently does not reuse HTTP connections even when Keep-Alive is
// requested. ngrok Free permits 100 new TCP connections per rolling minute, so
// the cover-art burst must stay comfortably below that ceiling.
static constexpr s64 HTTPS_OPEN_INTERVAL_MS = 700;
static s64 g_lastHttpsRequestMs = 0;

static bool isHttps(const std::string& url) {
    return url.size() >= 8 && url.compare(0, 8, "https://") == 0;
}

static bool isRedirect(u32 status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static void paceHttpsRequest(const std::string& url) {
    if (!isHttps(url)) return;
    s64 now = (s64)osGetTime();
    s64 waitMs = HTTPS_OPEN_INTERVAL_MS - (now - g_lastHttpsRequestMs);
    if (g_lastHttpsRequestMs != 0 && waitMs > 0)
        svcSleepThread(waitMs * 1000000LL);
    g_lastHttpsRequestMs = (s64)osGetTime();
}

static std::string redirectUrl(const std::string& current,
                               const std::string& location) {
    if (location.compare(0, 7, "http://") == 0 ||
        location.compare(0, 8, "https://") == 0)
        return location;

    size_t scheme = current.find("://");
    if (scheme == std::string::npos) return location;
    size_t authorityEnd = current.find('/', scheme + 3);
    std::string origin = authorityEnd == std::string::npos
                       ? current : current.substr(0, authorityEnd);

    if (location.compare(0, 2, "//") == 0)
        return current.substr(0, scheme + 1) + location;
    if (!location.empty() && location[0] == '/')
        return origin + location;

    size_t slash = current.rfind('/');
    if (slash == std::string::npos || slash < scheme + 3)
        return origin + "/" + location;
    return current.substr(0, slash + 1) + location;
}

static Result configureContext(httpcContext* ctx, const std::string& url,
                               const char* accept) {
    if (isHttps(url)) {
        Result ret = httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
        if (R_FAILED(ret)) return ret;
    }
    Result ret = httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
    if (R_FAILED(ret)) return ret;
    ret = httpcAddRequestHeaderField(ctx, "User-Agent", "3DSFin/0.2-emulator");
    if (R_FAILED(ret)) return ret;
    ret = httpcAddRequestHeaderField(ctx, "Accept", accept);
    if (R_FAILED(ret)) return ret;
    return httpcAddRequestHeaderField(ctx, "ngrok-skip-browser-warning", "1");
}

const char* httpFailureStageName(HttpFailureStage stage) {
    switch (stage) {
        case HttpFailureStage::None:         return "none";
        case HttpFailureStage::Open:         return "open";
        case HttpFailureStage::ConfigureTls: return "configure";
        case HttpFailureStage::Begin:        return "begin";
        case HttpFailureStage::Status:       return "status";
        case HttpFailureStage::Redirect:     return "redirect";
    }
    return "unknown";
}

HttpStreamResult httpOpenEmulatorStream(httpcContext* ctx,
                                        const std::string& url,
                                        const char* accept) {
    HttpStreamResult out;
    out.finalUrl = url;

    for (unsigned attempt = 0; attempt <= MAX_REDIRECTS; attempt++) {
        Result ret = httpcOpenContext(ctx, HTTPC_METHOD_GET,
                                      out.finalUrl.c_str(), 1);
        if (R_FAILED(ret)) {
            out.result = ret;
            out.stage = HttpFailureStage::Open;
            return out;
        }

        ret = configureContext(ctx, out.finalUrl, accept);
        if (R_FAILED(ret)) {
            httpcCloseContext(ctx);
            out.result = ret;
            out.stage = HttpFailureStage::ConfigureTls;
            return out;
        }

        paceHttpsRequest(out.finalUrl);
        ret = httpcBeginRequest(ctx);
        if (R_FAILED(ret)) {
            httpcCloseContext(ctx);
            out.result = ret;
            out.stage = HttpFailureStage::Begin;
            return out;
        }

        ret = httpcGetResponseStatusCode(ctx, &out.status);
        if (R_FAILED(ret)) {
            httpcCloseContext(ctx);
            out.result = ret;
            out.stage = HttpFailureStage::Status;
            return out;
        }

        if (!isRedirect(out.status)) {
            out.ok = out.status >= 200 && out.status < 300;
            if (!out.ok) httpcCloseContext(ctx);
            return out;
        }

        char location[2048] = {};
        ret = httpcGetResponseHeader(ctx, "Location", location,
                                     sizeof(location));
        httpcCloseContext(ctx);
        if (R_FAILED(ret) || location[0] == '\0' || attempt == MAX_REDIRECTS) {
            out.result = R_FAILED(ret) ? ret : (Result)-1;
            out.stage = HttpFailureStage::Redirect;
            return out;
        }
        out.finalUrl = redirectUrl(out.finalUrl, location);
        out.redirects++;
    }
    return out;
}

void HttpClient::setBaseUrl(const std::string& url) {
    baseUrl_ = url;
    while (!baseUrl_.empty() && baseUrl_.back() == '/')
        baseUrl_.pop_back();
}

void HttpClient::setHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void HttpClient::clearHeaders() {
    headers_.clear();
}

HttpResponse HttpClient::get(const std::string& path) {
    return request(baseUrl_ + path, Method::Get, "", "");
}

HttpResponse HttpClient::post(const std::string& path, const std::string& body,
                              const std::string& contentType) {
    return request(baseUrl_ + path, Method::Post, body, contentType);
}

HttpResponse HttpClient::del(const std::string& path) {
    return request(baseUrl_ + path, Method::Delete, "", "");
}

HttpResponse HttpClient::request(const std::string& url, Method method,
                                 const std::string& postData,
                                 const std::string& contentType) {
    HttpResponse resp{0, "", 0, HttpFailureStage::None};
    HTTPC_RequestMethod hm = method == Method::Post   ? HTTPC_METHOD_POST
                           : method == Method::Delete ? HTTPC_METHOD_DELETE
                                                      : HTTPC_METHOD_GET;
    std::string currentUrl = url;
    httpcContext ctx;
    u32 statuscode = 0;
    for (unsigned attempt = 0; ; attempt++) {
        Result ret = httpcOpenContext(&ctx, hm, currentUrl.c_str(), 1);
        if (R_FAILED(ret)) {
            resp.status = -1; resp.result = ret;
            resp.failureStage = HttpFailureStage::Open;
            return resp;
        }

        ret = configureContext(&ctx, currentUrl, "application/json");
        if (R_FAILED(ret)) {
            httpcCloseContext(&ctx);
            resp.status = -2; resp.result = ret;
            resp.failureStage = HttpFailureStage::ConfigureTls;
            return resp;
        }
        for (auto& kv : headers_)
            httpcAddRequestHeaderField(&ctx, kv.first.c_str(), kv.second.c_str());

        if (hm == HTTPC_METHOD_POST && !postData.empty()) {
            httpcAddRequestHeaderField(&ctx, "Content-Type", contentType.c_str());
            httpcAddPostDataRaw(&ctx,
                reinterpret_cast<const u32*>(postData.c_str()),
                static_cast<u32>(postData.size()));
        }

        paceHttpsRequest(currentUrl);
        ret = httpcBeginRequest(&ctx);
        if (R_FAILED(ret)) {
            httpcCloseContext(&ctx);
            resp.status = -2; resp.result = ret;
            resp.failureStage = HttpFailureStage::Begin;
            return resp;
        }

        ret = httpcGetResponseStatusCode(&ctx, &statuscode);
        if (R_FAILED(ret)) {
            httpcCloseContext(&ctx);
            resp.status = -2; resp.result = ret;
            resp.failureStage = HttpFailureStage::Status;
            return resp;
        }

        if (!isRedirect(statuscode)) break;
        char location[2048] = {};
        ret = httpcGetResponseHeader(&ctx, "Location", location,
                                     sizeof(location));
        httpcCloseContext(&ctx);
        if (R_FAILED(ret) || location[0] == '\0' || attempt == MAX_REDIRECTS) {
            resp.status = -2; resp.result = R_FAILED(ret) ? ret : (Result)-1;
            resp.failureStage = HttpFailureStage::Redirect;
            return resp;
        }
        currentUrl = redirectUrl(currentUrl, location);
        if (method == Method::Post && statuscode != 307 && statuscode != 308)
            hm = HTTPC_METHOD_GET;
    }
    resp.status = static_cast<int>(statuscode);

    // Stream response in 4 KB chunks
    u8 buf[4096];
    resp.body.reserve(16384);
    Result dlret;
    do {
        u32 read = 0;
        dlret = httpcDownloadData(&ctx, buf, sizeof(buf), &read);
        if (read > 0) {
            resp.body.append(reinterpret_cast<char*>(buf), read);
            if (resp.body.size() >= MAX_BODY) break;
        }
    } while (dlret == static_cast<Result>(HTTPC_RESULTCODE_DOWNLOADPENDING));

    httpcCloseContext(&ctx);
    return resp;
}
