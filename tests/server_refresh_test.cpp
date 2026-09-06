#include "jellyfin.h"
#include <cassert>
#include <deque>
#include <iostream>
struct Expected { std::string method, path; HttpResponse response; };
static std::deque<Expected> calls;
const char* httpFailureStageName(HttpFailureStage) { return "mock"; }
static HttpResponse take(const std::string& method, const std::string& path) {
    assert(!calls.empty()); auto c = calls.front(); calls.pop_front();
    assert(c.method == method && c.path == path); return c.response;
}
void HttpClient::setBaseUrl(const std::string&) {}
void HttpClient::setHeader(const std::string&, const std::string&) {}
void HttpClient::clearHeaders() {}
HttpResponse HttpClient::get(const std::string& p) { return take("GET", p); }
HttpResponse HttpClient::post(const std::string& p, const std::string& body, const std::string&) {
    if (p.find("/ScheduledTasks/Running/") == 0) assert(body == "{}");
    return take("POST", p);
}
HttpResponse HttpClient::del(const std::string& p) { return take("DELETE", p); }
static std::string tasks(const char* state="Idle") {
    return std::string("[{\"Id\":\"library\",\"Key\":\"RefreshLibrary\",\"State\":\"")+state+
        "\",\"LastExecutionResult\":{\"Status\":\"Completed\"}},"
        "{\"Id\":\"guide\",\"Key\":\"RefreshGuide\",\"State\":\"Idle\"}]";
}
int main() {
    JellyfinClient client;
    calls={{"GET","/ScheduledTasks",{200,tasks()}},{"POST","/ScheduledTasks/Running/library",{204,""}},
           {"POST","/ScheduledTasks/Running/guide",{204,""}}};
    auto result=client.refreshLibrariesAndGuide();
    assert(result.find("Libraries: started.")!=std::string::npos && result.find("Guide: started.")!=std::string::npos && calls.empty());
    calls={{"GET","/ScheduledTasks",{200,tasks("Running")}},{"POST","/ScheduledTasks/Running/guide",{204,""}}};
    assert(client.refreshLibrariesAndGuide().find("already running")!=std::string::npos && calls.empty());
    calls={{"GET","/ScheduledTasks",{403,""}}};
    assert(client.refreshLibrariesAndGuide().find("Admin permission required")!=std::string::npos && calls.empty());
    calls={{"GET","/ScheduledTasks",{200,tasks()}},{"POST","/ScheduledTasks/Running/library",{403,""}},
           {"POST","/ScheduledTasks/Running/guide",{204,""}}};
    result=client.refreshLibrariesAndGuide();
    assert(result.find("Admin permission required")!=std::string::npos && result.find("Guide: started.")!=std::string::npos && calls.empty());
    calls={{"GET","/ScheduledTasks",{200,"[]"}}};
    assert(client.refreshLibrariesAndGuide().find("task not found")!=std::string::npos && calls.empty());
    calls={{"GET","/ScheduledTasks",{0,""}}};
    assert(client.refreshLibrariesAndGuide().find("Server unreachable")!=std::string::npos && calls.empty());
    calls={{"GET","/ScheduledTasks",{200,tasks("Cancelling")}},{"POST","/ScheduledTasks/Running/guide",{204,""}}};
    assert(client.refreshLibrariesAndGuide().find("stopping; try again")!=std::string::npos && calls.empty());

    auto snapshot = [](const char* state, const char* percent, const char* end, const char* status) {
        std::string body = "[";
        for (auto id : {"library", "guide"}) {
            if (body.size() > 1) body += ",";
            body += std::string("{\"Id\":\"") + id + "\",\"Key\":\"" +
                (std::string(id) == "library" ? "RefreshLibrary" : "RefreshGuide") +
                "\",\"State\":\"" + state + "\",\"CurrentProgressPercentage\":" + percent +
                ",\"LastExecutionResult\":{\"EndTimeUtc\":\"" + end + "\",\"Status\":\"" + status + "\"}}";
        }
        return body + "]";
    };
    auto begin = [&]() {
        calls={{"GET","/ScheduledTasks",{200,snapshot("Idle","null","old","Completed")}},
               {"POST","/ScheduledTasks/Running/library",{204,""}},
               {"POST","/ScheduledTasks/Running/guide",{204,""}}};
        client.refreshLibrariesAndGuide();
        assert(client.scansActive() && !client.scansCompleted());
    };
    auto poll = [&](std::string body) {
        calls={{"GET","/ScheduledTasks",{200,body}}};
        client.pollScanProgress(); assert(calls.empty());
    };
    begin();
    poll(snapshot("Idle","null","old","Completed"));
    assert(client.scansActive() && !client.scansCompleted()); // stale completion
    poll(snapshot("Running","42.5","old","Completed"));
    assert(client.scanProgress()[0].percent == 42.5f);
    calls={{"GET","/ScheduledTasks",{0,""}}}; client.pollScanProgress();
    assert(client.scansActive() && client.scanProgress()[0].percent == -1);
    poll(snapshot("Running","null","old","Completed"));
    assert(client.scanProgress()[0].percent == -1);
    poll(snapshot("Running","150","old","Completed"));
    assert(client.scanProgress()[0].percent == 100 && !client.scansCompleted());
    poll(snapshot("Idle","null","new","Completed"));
    assert(!client.scansActive() && client.scansCompleted());
    client.pollScanProgress(); // completed scans must not request again
    begin();
    poll(snapshot("Idle","null","new","Completed")); // finished between polls
    assert(client.scansCompleted());
    begin();
    poll(snapshot("Idle","null","new","Failed"));
    assert(!client.scansCompleted() && !client.scansActive());
    begin();
    poll(snapshot("Idle","null","new","Cancelled"));
    assert(client.scanProgress()[0].message == "Cancelled" && !client.scansCompleted());
    begin(); poll("[]");
    assert(!client.scansActive() && !client.scansCompleted());
    begin(); calls={{"GET","/ScheduledTasks",{403,""}}}; client.pollScanProgress();
    assert(!client.scansActive() && !client.scansCompleted() && calls.empty());
    std::cout << "Scan start and progress regression checks passed\n";
}
