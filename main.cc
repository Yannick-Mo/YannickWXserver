#include <drogon/drogon.h>
#include "controllers/UserApi.h"      // 确保控制器被链接
#include "controllers/ChatWebSocket.h"

int main() {
    drogon::app().loadConfigFile("config.json");

    // 可选：检查 JWT 密钥配置
    auto &custom = drogon::app().getCustomConfig();
    if (!custom.isNull()) {
        auto jwtSecret = custom.get("jwt_secret", "").asString();
        if (jwtSecret.empty()) {
            LOG_ERROR << "JWT secret not configured!";
            exit(1);
        }
        LOG_INFO << "JWT secret loaded";
    }

    LOG_INFO << "Server started";
    drogon::app().run();
    return 0;
}