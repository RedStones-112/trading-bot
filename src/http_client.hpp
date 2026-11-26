#pragma once
#include <string>
#include <stdexcept>

// Minimal HTTPS client on WinHTTP (native Windows API, no external dependency).
// ponytail: no connection pooling / retry, one-shot request per call — add if polling rate makes it a bottleneck.
namespace http {

struct Response {
    int status = 0;
    std::string body;
    std::string trCont; // KIS 연속조회 응답 헤더 "tr_cont"(없으면 빈 문자열)
};

// method: "GET" or "POST". extraHeaders is a single CRLF-joined header block, e.g. "X: 1\r\nY: 2".
Response request(const std::wstring& host, int port, const std::wstring& path, const std::string& method,
                  const std::string& extraHeaders, const std::string& body);

} // namespace http
