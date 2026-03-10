#pragma once
#include "kmp/config.h"
#include <nlohmann/json.hpp>
#include <string>
#include <mutex>
#include <atomic>
#include <functional>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

namespace kmp {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    bool success = false;
};

class MasterHttpClient {
public:
    bool Init(const std::string& baseUrl, const std::string& apiKey);
    void Shutdown();

    bool Register(const ServerConfig& config, const std::string& externalIp);
    bool Heartbeat();
    bool UpdatePlayerCount(int currentPlayers);
    void Deregister();

    void Update(float deltaTime, int currentPlayers);

    bool IsRegistered() const { return m_registered; }
    const std::string& ServerId() const { return m_serverId; }

private:
    HttpResponse SendRequest(const std::string& method, const std::string& path,
                             const std::string& body = "", int timeoutMs = 10000);

    void ParseUrl(const std::string& url);

    HINTERNET m_session = nullptr;
    std::string m_host;
    int m_port = 80;
    bool m_useHttps = false;
    std::string m_apiKey;
    std::string m_serverId;
    std::atomic<bool> m_registered{false};
    bool m_authFailed = false;
    std::mutex m_mutex;

    ServerConfig m_cachedConfig;
    std::string m_cachedExternalIp;

    float m_heartbeatTimer = 0.f;
    float m_heartbeatInterval = 30.f;

    float m_reconnectTimer = 0.f;
    float m_reconnectDelay = 2.f;
    int m_consecutiveFailures = 0;

    float m_playerCountDebounceTimer = -1.f;
    int m_pendingPlayerCount = 0;
    int m_lastReportedPlayerCount = -1;
};

} // namespace kmp
