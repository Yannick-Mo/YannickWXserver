#include <drogon/drogon.h>
#include <filesystem>
#include <iostream>

#include "controllers/RegisterController.h"
#include "controllers/LoginController.h"
#include "controllers/UserApi.h"
#include "controllers/ChatWebSocket.h"
#include "controllers/FriendRequestCtrl.h"
#include "controllers/FileUploadCtrl.h"
#include "filters/JwtFilter.h"

namespace fs = std::filesystem;

int main() {
    // 获取工作目录
    const fs::path cwd = fs::current_path();
    LOG_INFO << "Server working directory: " << cwd.string();

    // 常量定义：上传临时目录
    constexpr std::string_view DROGON_UPLOAD_PATH = "./uploads";
    try {
        const fs::path abs_upload_path = fs::absolute(DROGON_UPLOAD_PATH);
        // 创建目录（不存在则创建）
        if (!fs::exists(abs_upload_path)) {
            fs::create_directories(abs_upload_path);
            LOG_INFO << "Created upload temp directory: " << abs_upload_path.string();
        }
        // 校验目录合法性
        if (!fs::is_directory(abs_upload_path)) {
            LOG_ERROR << "Upload path is not a directory: " << abs_upload_path.string();
            return 1;
        }
    } catch (const std::exception& e) {
        LOG_FATAL << "Failed to init upload temp directory: " << e.what();
        return 1;
    }

    // 加载服务配置
    drogon::app().loadConfigFile("config.json");

    // 初始化最终文件存储目录
    const auto& custom_config = drogon::app().getCustomConfig();
    constexpr std::string_view DEFAULT_FINAL_DIR = "./uploads/files";
    try {
        if (!custom_config.isNull() && custom_config.isMember("upload") && custom_config["upload"].isMember("upload_dir")) {
            // 读取配置中的存储路径
            fs::path final_path = custom_config["upload"]["upload_dir"].asString();
            if (final_path.is_relative()) {
                final_path = cwd / final_path;
            }
            // 创建并校验目录
            if (!fs::exists(final_path)) {
                fs::create_directories(final_path);
                LOG_INFO << "Created final storage directory: " << final_path.string();
            }
            if (!fs::is_directory(final_path)) {
                LOG_ERROR << "Final storage path is not a directory: " << final_path.string();
                return 1;
            }
        } else {
            LOG_WARN << "Custom config 'upload.upload_dir' not found, use default path";
            // 创建默认存储目录
            const fs::path abs_default_path = fs::absolute(DEFAULT_FINAL_DIR);
            if (!fs::exists(abs_default_path)) {
                fs::create_directories(abs_default_path);
            }
        }
    } catch (const std::exception& e) {
        LOG_FATAL << "Failed to init final storage directory: " << e.what();
        return 1;
    }

    // JWT密钥校验
    if (custom_config.isNull()) {
        LOG_ERROR << "Custom config is missing!";
        exit(1);
    }
    const std::string jwt_secret = custom_config.get("jwt_secret", "").asString();
    if (jwt_secret.empty()) {
        LOG_ERROR << "JWT secret is not configured!";
        exit(1);
    }
    LOG_INFO << "JWT secret loaded successfully";

    // 启动服务
    LOG_INFO << "Server initialized, starting Drogon...";
    drogon::app().run();
    return 0;
}