#include "RegisterController.h"
#include <drogon/orm/DbClient.h>
#include <models/User.h>
#include "utils/PasswordHelper.h"
#include <random>
#include <iomanip>
#include <sstream>

using namespace drogon;
using namespace drogon::orm;
using namespace api::v1;
using namespace drogon_model::yaroserverdb;

namespace
{
    // 生成随机用户名（前缀 + 8位字母数字）
    std::string generateRandomUsername()
    {
        static const char alphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

        std::ostringstream oss;
        oss << "user_";
        for (int i = 0; i < 8; ++i)
            oss << alphanum[dis(gen)];
        return oss.str();
    }

    // 插入自身好友记录（完全匹配friend表结构，无任何错误）
    void insertSelfFriendRecord(const DbClientPtr &db, int64_t userId, const std::string &nickname)
    {
        // 核心优化：
        // 1. tags用JSON空数组'[]'（符合JSON类型+标签数组设计）
        // 2. status用0（匹配表注释：0-正常好友）
        // 3. 所有字段类型严格匹配（TINYINT/INT/BIGINT/VARCHAR/JSON）
        std::string sql = R"(
            INSERT INTO friend (
                user_id, friend_id, remark, description, tags, 
                phone_note, email_note, source, is_starred, 
                is_blocked, status, created_at, updated_at
            ) VALUES (?, ?, ?, '', '[]', '', '', ?, 0, 0, 0, NOW(), NOW())
        )";

        db->execSqlAsync(
            sql,
            [userId, nickname](const Result &) {
                LOG_INFO << "User " << userId << " self friend record inserted successfully, remark: " << nickname;
            },
            [userId](const DrogonDbException &e) {
                // 捕获唯一索引冲突（避免重复插入自身好友）
                std::string err = e.base().what();
                if (err.find("Duplicate entry") != std::string::npos && err.find("uk_user_friend") != std::string::npos) {
                    LOG_WARN << "User " << userId << " self friend record already exists (ignore)";
                } else {
                    LOG_ERROR << "Insert self friend failed for user " << userId 
                              << ": " << e.base().what();
                }
            },
            userId,          // user_id (BIGINT)
            userId,          // friend_id (BIGINT)
            nickname,        // remark (VARCHAR) - 用户昵称
            "self_register"  // source (VARCHAR) - 注册自动添加
        );
    }

    // 递归尝试插入手机号注册（处理可能的用户名冲突）
    void tryInsertPhoneUser(const std::function<void(const HttpResponsePtr &)> &callback,
                            const DbClientPtr &db,
                            const std::string &phone,
                            const std::string &passwordHash,
                            const std::string &nickname,
                            int retryCount = 0)
    {
        const int maxRetries = 5;
        std::string username = generateRandomUsername();

        // 构建插入SQL（phone不为空）
        std::string sql = "INSERT INTO user (username, phone, password_hash, nickname) VALUES (?, ?, ?, ?)";

        db->execSqlAsync(
            sql,
            [callback, db, nickname](const Result &r) {
                // 手机号注册成功，返回响应+插入自身好友
                Json::Value ret;
                ret["success"] = true;
                ret["user_id"] = static_cast<Json::Int64>(r.insertId());
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k201Created);
                callback(resp);

                // 插入自身好友记录（传用户ID+昵称）
                insertSelfFriendRecord(db, r.insertId(), nickname);
            },
            [callback, db, phone, passwordHash, nickname, retryCount](const DrogonDbException &e) {
                std::string err = e.base().what();

                // 检查是否为用户名唯一性冲突
                if (err.find("Duplicate entry") != std::string::npos &&
                    err.find("username") != std::string::npos)
                {
                    if (retryCount < maxRetries)
                    {
                        // 重试（重新生成用户名）
                        tryInsertPhoneUser(callback, db, phone, passwordHash, nickname, retryCount + 1);
                    }
                    else
                    {
                        Json::Value ret;
                        ret["success"] = false;
                        ret["error"] = "Failed to generate unique username after multiple attempts";
                        auto resp = HttpResponse::newHttpJsonResponse(ret);
                        resp->setStatusCode(k500InternalServerError);
                        callback(resp);
                    }
                }
                else if (err.find("Duplicate entry") != std::string::npos &&
                         err.find("phone") != std::string::npos)
                {
                    // 手机号已存在
                    Json::Value ret;
                    ret["success"] = false;
                    ret["error"] = "Phone number already registered";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k409Conflict);
                    callback(resp);
                }
                else
                {
                    LOG_ERROR << "DB error (phone register): " << err;
                    Json::Value ret;
                    ret["success"] = false;
                    ret["error"] = "Internal server error";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k500InternalServerError);
                    callback(resp);
                }
            },
            username, phone, passwordHash, nickname
        );
    }
}

void RegisterController::reg(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json)
    {
        Json::Value ret;
        ret["success"] = false;
        ret["error"] = "Invalid JSON";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // 检查密码
    if (!(*json)["password"].isString() || (*json)["password"].asString().empty())
    {
        Json::Value ret;
        ret["success"] = false;
        ret["error"] = "Missing password";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    bool hasUsername = json->isMember("username") && (*json)["username"].isString() && !(*json)["username"].asString().empty();
    bool hasPhone = json->isMember("phone") && (*json)["phone"].isString() && !(*json)["phone"].asString().empty();

    // 必须提供username或phone之一，但不能同时提供
    if (!hasUsername && !hasPhone)
    {
        Json::Value ret;
        ret["success"] = false;
        ret["error"] = "Either username or phone is required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }
    if (hasUsername && hasPhone)
    {
        Json::Value ret;
        ret["success"] = false;
        ret["error"] = "Please provide only one of username or phone";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string password = (*json)["password"].asString();
    std::string nickname = json->get("nickname", "").asString();
    if (nickname.empty())
    {
        // 无昵称时，用用户名/手机号填充
        nickname = hasUsername ? (*json)["username"].asString() : (*json)["phone"].asString();
    }

    // 生成密码哈希
    std::string hash = PasswordHelper::generateHash(password);
    if (hash.empty())
    {
        Json::Value ret;
        ret["success"] = false;
        ret["error"] = "Failed to generate password hash";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    auto db = app().getDbClient("serverDb");

    if (hasUsername)
    {
        // 用户名注册
        std::string username = (*json)["username"].asString();
        std::string sql = "INSERT INTO user (username, password_hash, nickname) VALUES (?, ?, ?)";

        db->execSqlAsync(
            sql,
            [callback, db, nickname](const Result &r) {
                // 用户名注册成功，返回响应+插入自身好友
                Json::Value ret;
                ret["success"] = true;
                ret["user_id"] = static_cast<Json::Int64>(r.insertId());
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k201Created);
                callback(resp);

                // 插入自身好友记录（传用户ID+昵称）
                insertSelfFriendRecord(db, r.insertId(), nickname);
            },
            [callback](const DrogonDbException &e) {
                std::string err = e.base().what();
                if (err.find("Duplicate entry") != std::string::npos &&
                    err.find("username") != std::string::npos)
                {
                    Json::Value ret;
                    ret["success"] = false;
                    ret["error"] = "Username already exists";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k409Conflict);
                    callback(resp);
                }
                else
                {
                    LOG_ERROR << "DB error (username register): " << err;
                    Json::Value ret;
                    ret["success"] = false;
                    ret["error"] = "Internal server error";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    resp->setStatusCode(k500InternalServerError);
                    callback(resp);
                }
            },
            username, hash, nickname
        );
    }
    else // hasPhone
    {
        // 手机号注册
        std::string phone = (*json)["phone"].asString();
        tryInsertPhoneUser(callback, db, phone, hash, nickname);
    }
}