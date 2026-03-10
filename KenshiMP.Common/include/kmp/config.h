#pragma once
#include "constants.h"
#include <string>
#include <vector>
#include <cstdint>

namespace kmp {

struct ClientConfig {
    std::string playerName     = "Player";
    std::string lastServer     = "";
    uint16_t    lastPort       = KMP_DEFAULT_PORT;
    bool        autoConnect    = false;
    float       overlayScale   = 1.0f;
    std::string masterServerUrl = "http://localhost:3000";
    std::vector<std::string> favoriteServers = {};
    bool        useSyncOrchestrator = false;

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    static std::string GetDefaultPath();
};

struct ServerConfig {
    std::string serverName   = "KenshiMP Server";
    uint16_t    port         = KMP_DEFAULT_PORT;
    int         maxPlayers   = KMP_MAX_PLAYERS;
    std::string password;
    std::string savePath     = "world.kmpsave";
    int         tickRate     = KMP_TICK_RATE;
    bool        pvpEnabled   = true;
    float       gameSpeed    = 1.0f;
    std::string masterServerUrl = "http://localhost:3000";
    std::string masterServerApiKey;

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace kmp
