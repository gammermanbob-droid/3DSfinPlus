#pragma once
#include <string>
#include <map>
#include <3ds.h>

enum class HttpFailureStage {
    None,
    Open,
    ConfigureTls,
    Begin,
    Status,
    Redirect,
};

struct HttpStreamResult {
    bool             ok        = false;
    u32              status    = 0;
    Result           result    = 0;
    HttpFailureStage stage     = HttpFailureStage::None;
    unsigned         redirects = 0;
    std::string      finalUrl;
};

// Opens a GET response and leaves ctx open only when a 2xx response is ready
// to download. HTTPS certificate verification is disabled for emulator
// compatibility; do not use this build with untrusted endpoints.
HttpStreamResult httpOpenEmulatorStream(httpcContext* ctx,
                                        const std::string& url,
                                        const char* accept = "*/*");

const char* httpFailureStageName(HttpFailureStage stage);

struct HttpResponse {
    int         status;
    std::string body;
    Result      result = 0;
    HttpFailureStage failureStage = HttpFailureStage::None;
    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    void setBaseUrl(const std::string& url);
    void setHeader(const std::string& key, const std::string& value);
    void clearHeaders();

    HttpResponse get(const std::string& path);
    HttpResponse post(const std::string& path, const std::string& body,
                      const std::string& contentType = "application/json");
    HttpResponse del(const std::string& path);

private:
    enum class Method { Get, Post, Delete };

    std::string                        baseUrl_;
    std::map<std::string, std::string> headers_;

    HttpResponse request(const std::string& url, Method method,
                         const std::string& postData,
                         const std::string& contentType);
};
