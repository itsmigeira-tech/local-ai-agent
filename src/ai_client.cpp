#include "ai_client.h"
#include "json.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace {

const int kOllamaPort = 11434;
const int kOpenAIPort = 1234; // LM Studio default

std::string BackendKindName(int kind) {
    switch (kind) {
        case BK_OLLAMA: return "Ollama";
        case BK_OPENAI: return "LM Studio / OpenAI";
        default: return "Auto";
    }
}

} // namespace

AIClient::AIClient(const std::string& host)
    : m_host(host) {
}

AIClient::~AIClient() {
}

int AIClient::DetectPort(int backendKind) {
    return backendKind == BK_OLLAMA ? kOllamaPort : kOpenAIPort;
}

std::string AIClient::EscapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool AIClient::Detect(int backendKind, std::string& errorOut) {
    int candidates[2];
    int count = 0;
    if (backendKind == BK_AUTO) {
        candidates[0] = BK_OLLAMA;
        candidates[1] = BK_OPENAI;
        count = 2;
    } else {
        candidates[0] = backendKind;
        count = 1;
    }

    for (int i = 0; i < count; ++i) {
        int kind = candidates[i];
        int port = DetectPort(kind);
        std::string endpoint = (kind == BK_OLLAMA) ? "/api/tags" : "/v1/models";
        std::string resp, err;
        if (!HttpRequest("GET", port, endpoint, "", resp, err)) {
            errorOut = err;
            continue;
        }

        std::string perr;
        json::Value root = json::Parse(resp, perr);
        bool ok = root.IsObject() && (root.Find(kind == BK_OLLAMA ? "models" : "data") != nullptr);
        if (ok) {
            m_activeBackend = kind;
            m_activePort = port;
            errorOut.clear();
            return true;
        }
        errorOut = BackendKindName(kind) + " responded with an unexpected payload.";
    }

    m_activeBackend = 0;
    m_activePort = 0;
    if (errorOut.empty()) errorOut = "No local AI backend detected.";
    return false;
}

std::vector<ModelInfo> AIClient::GetModels(int backendKind, std::string& backendLabel, std::string& errorOut) {
    std::vector<ModelInfo> models;
    backendLabel.clear();
    errorOut.clear();

    if (!Detect(backendKind, errorOut)) {
        backendLabel = "None";
        return models;
    }

    std::string resp, err;
    std::string endpoint = (m_activeBackend == BK_OLLAMA) ? "/api/tags" : "/v1/models";
    if (!HttpRequest("GET", m_activePort, endpoint, "", resp, err)) {
        errorOut = err;
        return models;
    }

    std::string perr;
    json::Value root = json::Parse(resp, perr);
    if (!root.IsObject()) {
        errorOut = "Unrecognized response from " + BackendKindName(m_activeBackend) + ".";
        return models;
    }

    if (m_activeBackend == BK_OLLAMA) {
        backendLabel = "Ollama";
        const json::Value* arr = root.Find("models");
        if (arr && arr->IsArray()) {
            for (const json::Value& m : arr->array) {
                const json::Value* name = m.Find("name");
                if (name && name->IsString() && !name->Str().empty()) {
                    models.push_back(ModelInfo{name->Str()});
                }
            }
        }
    } else {
        backendLabel = "LM Studio / OpenAI";
        const json::Value* arr = root.Find("data");
        if (arr && arr->IsArray()) {
            for (const json::Value& m : arr->array) {
                const json::Value* id = m.Find("id");
                if (id && id->IsString() && !id->Str().empty()) {
                    models.push_back(ModelInfo{id->Str()});
                }
            }
        }
    }

    return models;
}

std::string AIClient::Chat(const std::string& modelName, const std::string& prompt,
                           int backendKind, int port, std::string& errorOut) {
    errorOut.clear();
    if (backendKind != BK_OLLAMA && backendKind != BK_OPENAI) {
        errorOut = "No local AI backend connected. Start Ollama or LM Studio first.";
        return "";
    }

    std::string body = "{\"model\":\"" + EscapeJson(modelName) +
                       "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + EscapeJson(prompt) +
                       "\"}],\"stream\":false}";
    std::string endpoint = (backendKind == BK_OLLAMA) ? "/api/chat" : "/v1/chat/completions";

    std::string resp;
    if (!HttpRequest("POST", port, endpoint, body, resp, errorOut)) {
        return "";
    }

    std::string perr;
    json::Value root = json::Parse(resp, perr);
    if (!root.IsObject()) {
        errorOut = "Invalid JSON response from model.";
        return resp;
    }

    const json::Value* errorVal = root.Find("error");
    if (errorVal) {
        if (errorVal->IsString()) errorOut = errorVal->Str();
        else errorOut = "Model error: " + resp;
        return "";
    }

    if (backendKind == BK_OLLAMA) {
        const json::Value* msg = root.Find("message");
        if (msg && msg->IsObject()) {
            const json::Value* content = msg->Find("content");
            if (content && content->IsString()) return content->Str();
        }
    } else {
        const json::Value* choices = root.Find("choices");
        if (choices && choices->IsArray() && choices->ArraySize() > 0) {
            const json::Value* msg = choices->At(0)->Find("message");
            if (msg && msg->IsObject()) {
                const json::Value* content = msg->Find("content");
                if (content && content->IsString()) return content->Str();
            }
        }
    }

    errorOut = "Could not extract reply from " + BackendKindName(backendKind) + ".";
    return resp;
}

bool AIClient::HttpRequest(const std::string& method, int port, const std::string& endpoint,
                           const std::string& body, std::string& responseOut, std::string& errorOut) {
    errorOut.clear();
    responseOut.clear();

    HINTERNET hSession = WinHttpOpen(L"LocalAIAgent/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        errorOut = "WinHttpOpen failed.";
        return false;
    }

    std::wstring wHost(m_host.begin(), m_host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        errorOut = "Could not connect to local AI server on port " + std::to_string(port) +
                   ". Make sure Ollama or LM Studio is running.";
        return false;
    }

    std::wstring wMethod(method.begin(), method.end());
    std::wstring wEndpoint(endpoint.begin(), endpoint.end());
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.c_str(), wEndpoint.c_str(),
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        errorOut = "WinHttpOpenRequest failed.";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Generous timeouts: loading a local model into memory can take a while.
    WinHttpSetTimeouts(hRequest, 10000, 10000, 60000, 600000);

    std::wstring headers = L"Content-Type: application/json\r\n";
    BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L,
                                       (LPVOID)(body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data()),
                                       (DWORD)body.size(), (DWORD)body.size(), 0);
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (!bResults) {
        DWORD err = GetLastError();
        errorOut = "HTTP request failed (error " + std::to_string(err) +
                   "). Is the local AI server running on port " + std::to_string(port) + "?";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Read status code so connection probes don't mistake 404/500 bodies for success.
    DWORD statusCode = 0;
    DWORD statusLen = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);

    std::string response;
    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> buffer(dwSize + 1, 0);
        if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
            response.append(buffer.data(), dwDownloaded);
        } else {
            break;
        }
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (statusCode >= 400) {
        errorOut = "HTTP " + std::to_string(statusCode) + ": " + response.substr(0, 512);
        return false;
    }

    responseOut = response;
    return true;
}