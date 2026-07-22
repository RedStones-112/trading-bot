#include "http_client.hpp"
#include <windows.h>
#include <winhttp.h>

namespace http {

Response request(const std::wstring& host, int port, const std::wstring& path, const std::string& method,
                  const std::string& extraHeaders, const std::string& body) {
    Response resp;

    HINTERNET hSession = WinHttpOpen(L"trading-bot/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) throw std::runtime_error("WinHttpOpen failed");
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000); // resolve/connect/send/receive, ms

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpConnect failed"); }

    std::wstring wmethod(method.begin(), method.end());
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wmethod.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpOpenRequest failed"); }

    std::wstring whdrs(extraHeaders.begin(), extraHeaders.end());
    BOOL ok = WinHttpSendRequest(hRequest,
        whdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whdrs.c_str(),
        whdrs.empty() ? 0 : -1L,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

    DWORD lastError = ok ? 0 : GetLastError();

    if (ok) {
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        resp.status = (int)statusCode;

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
            chunk.resize(read);
            resp.body += chunk;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok) throw std::runtime_error("WinHTTP request failed, GetLastError=" + std::to_string(lastError));
    return resp;
}

} // namespace http
