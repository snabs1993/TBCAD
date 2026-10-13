#pragma once

#include <functional>
#include <map>
#include <string>

#include "tbcad/core/result.h"

namespace tbcad::mcp {

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers{};  // lower-case names
    std::string body{};
};

// Minimal HTTP/1.1 client for 127.0.0.1 (one connection per request). Used by the stdio bridge and
// by tests.
class HttpClient {
public:
    explicit HttpClient(int port) : port_(port) {}

    Result<HttpResponse> request(const std::string& method, const std::string& path,
                                 const std::map<std::string, std::string>& headers,
                                 const std::string& body, int timeoutMs = 30000);

    // Opens a GET event stream and calls onEvent with the data of each `data:` event until the
    // server closes the stream, onEvent returns false or `keepGoing` returns false.
    Result<int> stream(const std::string& path, const std::map<std::string, std::string>& headers,
                       const std::function<bool(const std::string&)>& onEvent,
                       const std::function<bool()>& keepGoing);

private:
    int port_;
};

}  // namespace tbcad::mcp
