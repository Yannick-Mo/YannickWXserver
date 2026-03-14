#include <drogon/drogon.h>

// 包含自定义控制器和过滤器头文件（这些头文件是必需的，否则链接时可能找不到类定义）
#include "controllers/RegisterController.h"
#include "controllers/LoginController.h"
#include "controllers/UserApi.h"
#include "controllers/ChatWebSocket.h"
#include "filters/JwtFilter.h"

int main() {
    // 加载配置文件
    drogon::app().loadConfigFile("config.json");

    // 检查 JWT 密钥配置（可选）
    auto &custom = drogon::app().getCustomConfig();
    if (!custom.isNull()) {
        auto jwtSecret = custom.get("jwt_secret", "").asString();
        if (jwtSecret.empty()) {
            LOG_ERROR << "JWT secret not configured!";
            exit(1);
        }
        LOG_INFO << "JWT secret loaded";
    }

    // 注意：所有从 HttpController、WebSocketController、HttpFilter 派生的类会自动注册，
    // 因此不需要手动调用 registerController 或 registerFilter。

    LOG_INFO << "Server started";
    drogon::app().run();
    return 0;
}