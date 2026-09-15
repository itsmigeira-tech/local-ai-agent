#pragma once

#include <string>
#include <vector>

struct ModelInfo {
    std::string name;
};

// Which local inference backend to talk to.
enum BackendKind {
    BK_AUTO = 0,  // Auto-detect: Ollama first, then OpenAI-compatible (LM Studio)
    BK_OLLAMA = 1,
    BK_OPENAI = 2,  // OpenAI-compatible servers (LM Studio, llama.cpp server, ...)
};

class AIClient {
public:
    AIClient(const std::string& host = "127.0.0.1");
    ~AIClient();

    // Detect the selected backend and return its installed (already downloaded)
    // models. Never downloads or installs models itself.
    std::vector<ModelInfo> GetModels(int backendKind, std::string& backendLabel, std::string& errorOut);

    // Send one chat turn. backendKind/port must have been filled in by a
    // previous Detect()/GetModels() call.
    std::string Chat(const std::string& modelName, const std::string& prompt,
                     int backendKind, int port, std::string& errorOut);

    // Which backend was last detected (BK_OLLAMA or BK_OPENAI), or 0 if none.
    int ActiveBackend() const { return m_activeBackend; }

    // Default port for a backend kind.
    static int DetectPort(int backendKind);

private:
    std::string m_host;
    int m_activePort = 0;
    int m_activeBackend = 0;

    bool Detect(int backendKind, std::string& errorOut);
    bool HttpRequest(const std::string& method, int port, const std::string& endpoint,
                     const std::string& body, std::string& responseOut, std::string& errorOut);
    static std::string EscapeJson(const std::string& s);
};