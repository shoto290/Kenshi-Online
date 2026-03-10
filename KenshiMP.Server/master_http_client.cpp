#include "master_http_client.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <regex>

#pragma comment(lib, "winhttp.lib")

namespace kmp {

void MasterHttpClient::ParseUrl(const std::string& url) {
    std::string work = url;

    if (work.rfind("https://", 0) == 0) {
        m_useHttps = true;
        work = work.substr(8);
    } else if (work.rfind("http://", 0) == 0) {
        m_useHttps = false;
        work = work.substr(7);
    }

    m_port = m_useHttps ? 443 : 80;

    auto slashPos = work.find('/');
    std::string hostPort = (slashPos != std::string::npos) ? work.substr(0, slashPos) : work;

    auto colonPos = hostPort.find(':');
    if (colonPos != std::string::npos) {
        m_host = hostPort.substr(0, colonPos);
        m_port = std::stoi(hostPort.substr(colonPos + 1));
    } else {
        m_host = hostPort;
    }
}

bool MasterHttpClient::Init(const std::string& baseUrl, const std::string& apiKey) {
    m_apiKey = apiKey;
    ParseUrl(baseUrl);

    m_session = WinHttpOpen(L"KenshiMP/1.0",
                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session) {
        spdlog::error("MasterHttpClient: WinHttpOpen failed ({})", GetLastError());
        return false;
    }

    spdlog::info("MasterHttpClient: Initialized for {}://{}:{}", m_useHttps ? "https" : "http", m_host, m_port);
    return true;
}

void MasterHttpClient::Shutdown() {
    if (m_session) {
        WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
    m_registered = false;
    m_serverId.clear();
}

HttpResponse MasterHttpClient::SendRequest(const std::string& method, const std::string& path,
                                            const std::string& body, int timeoutMs) {
    HttpResponse response;

    if (!m_session) return response;

    std::wstring wHost(m_host.begin(), m_host.end());
    HINTERNET hConnect = WinHttpConnect(m_session, wHost.c_str(),
                                         static_cast<INTERNET_PORT>(m_port), 0);
    if (!hConnect) return response;

    std::wstring wMethod(method.begin(), method.end());
    std::wstring wPath(path.begin(), path.end());

    DWORD flags = m_useHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.c_str(), wPath.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        return response;
    }

    WinHttpSetTimeouts(hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!m_apiKey.empty()) {
        std::string authHeader = "Authorization: Bearer " + m_apiKey + "\r\n";
        headers += std::wstring(authHeader.begin(), authHeader.end());
    }

    LPVOID bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.c_str();
    DWORD bodyLen = body.empty() ? 0 : static_cast<DWORD>(body.size());

    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.size()),
                                    bodyPtr, bodyLen, bodyLen, 0);
    if (!sent) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return response;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = static_cast<int>(statusCode);

    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead);
        responseBody.append(buffer.data(), bytesRead);
    }
    response.body = responseBody;
    response.success = (statusCode >= 200 && statusCode < 300);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return response;
}

bool MasterHttpClient::Register(const ServerConfig& config, const std::string& externalIp) {
    std::lock_guard lock(m_mutex);

    m_cachedConfig = config;
    m_cachedExternalIp = externalIp;

    nlohmann::json payload;
    payload["name"] = config.serverName;
    payload["ip"] = externalIp;
    payload["port"] = config.port;
    payload["maxPlayers"] = config.maxPlayers;
    payload["gameVersion"] = config.gameVersion;
    payload["region"] = config.region;

    auto resp = SendRequest("POST", "/servers/register", payload.dump());

    if (resp.success) {
        try {
            auto j = nlohmann::json::parse(resp.body);
            m_serverId = j["id"].get<std::string>();
            m_registered = true;
            m_consecutiveFailures = 0;
            m_reconnectDelay = 2.f;
            m_reconnectTimer = 0.f;
            spdlog::info("MasterHttpClient: Registered as '{}' (id={})", config.serverName, m_serverId);
            return true;
        } catch (...) {
            spdlog::error("MasterHttpClient: Failed to parse registration response");
        }
    } else if (resp.statusCode == 401) {
        spdlog::critical("MasterHttpClient: Invalid API key — stopping master server communication");
        m_authFailed = true;
    } else {
        spdlog::warn("MasterHttpClient: Registration failed (HTTP {})", resp.statusCode);
    }

    m_consecutiveFailures++;
    m_reconnectDelay = std::min(m_reconnectDelay * 2.f, 60.f);
    m_reconnectTimer = 0.f;

    return false;
}

bool MasterHttpClient::Heartbeat() {
    std::lock_guard lock(m_mutex);

    if (!m_registered || m_serverId.empty()) return false;

    auto resp = SendRequest("POST", "/servers/" + m_serverId + "/heartbeat");

    if (resp.success) {
        m_consecutiveFailures = 0;
        return true;
    }

    m_consecutiveFailures++;

    if (resp.statusCode == 404) {
        spdlog::warn("MasterHttpClient: Server pruned (404) — will re-register");
        m_registered = false;
        m_serverId.clear();
        m_reconnectTimer = 0.f;
        m_reconnectDelay = 2.f;
    } else if (resp.statusCode == 401) {
        spdlog::critical("MasterHttpClient: Invalid API key on heartbeat");
        m_authFailed = true;
    } else {
        if (m_consecutiveFailures >= 3) {
            spdlog::warn("MasterHttpClient: {} consecutive heartbeat failures", m_consecutiveFailures);
        }
    }

    return false;
}

bool MasterHttpClient::UpdatePlayerCount(int currentPlayers) {
    std::lock_guard lock(m_mutex);

    if (!m_registered || m_serverId.empty()) return false;

    nlohmann::json payload;
    payload["currentPlayers"] = currentPlayers;

    auto resp = SendRequest("PATCH", "/servers/" + m_serverId, payload.dump());

    if (resp.success) {
        m_lastReportedPlayerCount = currentPlayers;
        return true;
    }

    if (resp.statusCode == 404) {
        spdlog::warn("MasterHttpClient: Server not found on player count update — will re-register");
        m_registered = false;
        m_serverId.clear();
    }

    return false;
}

void MasterHttpClient::Deregister() {
    std::lock_guard lock(m_mutex);

    if (!m_registered || m_serverId.empty()) return;

    SendRequest("DELETE", "/servers/" + m_serverId, "", 3000);
    spdlog::info("MasterHttpClient: Deregistered server {}", m_serverId);

    m_registered = false;
    m_serverId.clear();
}

void MasterHttpClient::Update(float deltaTime, int currentPlayers) {
    if (!m_session || m_authFailed) return;

    if (!m_registered) {
        m_reconnectTimer += deltaTime;
        if (m_reconnectTimer >= m_reconnectDelay && !m_cachedConfig.serverName.empty()) {
            m_reconnectTimer = 0.f;
            Register(m_cachedConfig, m_cachedExternalIp);
        }
        return;
    }

    m_heartbeatTimer += deltaTime;
    if (m_heartbeatTimer >= m_heartbeatInterval) {
        Heartbeat();
        m_heartbeatTimer = 0.f;
    }

    if (currentPlayers != m_lastReportedPlayerCount) {
        if (m_playerCountDebounceTimer < 0.f) {
            m_playerCountDebounceTimer = 0.f;
            m_pendingPlayerCount = currentPlayers;
        } else {
            m_pendingPlayerCount = currentPlayers;
        }
    }

    if (m_playerCountDebounceTimer >= 0.f) {
        m_playerCountDebounceTimer += deltaTime;
        if (m_playerCountDebounceTimer >= 2.f) {
            UpdatePlayerCount(m_pendingPlayerCount);
            m_playerCountDebounceTimer = -1.f;
        }
    }
}

} // namespace kmp
