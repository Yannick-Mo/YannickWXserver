#include "RegisterController.h"
#include <drogon/orm/DbClient.h>
#include <models/User.h>
#include "utils/PasswordHelper.h"   // 使用密码工具类

using namespace drogon;
using namespace drogon::orm;
using namespace api::v1;
using namespace drogon_model::yaroserverdb;

void RegisterController::reg(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json || !(*json)["username"].isString() || !(*json)["password"].isString())
    {
        Json::Value ret;
        ret["error"] = "Missing username or password";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string username = (*json)["username"].asString();
    std::string password = (*json)["password"].asString();
    std::string nickname = (*json).get("nickname", username).asString(); // 可选

    // 使用 PasswordHelper 生成哈希
    std::string hash = PasswordHelper::generateHash(password);
    if (hash.empty())
    {
        Json::Value ret;
        ret["error"] = "Failed to generate password hash";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    auto db = app().getDbClient("serverDb");

    // 插入数据库
    db->execSqlAsync(
        "INSERT INTO user (username, password_hash, nickname) VALUES (?, ?, ?)",
        [callback](const Result &r) {
            Json::Value ret;
            ret["message"] = "Register success";
            ret["user_id"] = static_cast<Json::Int64>(r.insertId());
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k201Created);
            callback(resp);
        },
        [callback](const DrogonDbException &e) {
            std::string err = e.base().what();
            if (err.find("Duplicate entry") != std::string::npos)
            {
                Json::Value ret;
                ret["error"] = "Username already exists";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k409Conflict);
                callback(resp);
            }
            else
            {
                LOG_ERROR << "DB error: " << err;
                Json::Value ret;
                ret["error"] = "Internal server error";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k500InternalServerError);
                callback(resp);
            }
        },
        username, hash, nickname
    );
}