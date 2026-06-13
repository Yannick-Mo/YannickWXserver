#include "LoginController.h"
#include <drogon/orm/DbClient.h>
#include <jwt-cpp/jwt.h>
#include <models/User.h>
#include "utils/PasswordHelper.h"
#include "utils/RedisCache.h"
#include <drogon/orm/Criteria.h>

using namespace drogon;
using namespace drogon::orm;
using namespace api::v1;
using namespace drogon_model::yaroserverdb;

static constexpr int CACHE_TTL = 3600;

void LoginController::login(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json || !(*json)["account"].isString() || !(*json)["password"].isString())
    {
        Json::Value ret;
        ret["error"] = "Missing account or password";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string account = (*json)["account"].asString();
    std::string password = (*json)["password"].asString();

    std::string jwtSecret = app().getCustomConfig()["jwt_secret"].asString();
    if (jwtSecret.empty()) {
        Json::Value ret;
        ret["error"] = "Server configuration error";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
        return;
    }

    // === Redis 缓存查询 ===
    std::string cacheKey = "login:" + account;
    // === MySQL ===
    auto db = app().getDbClient("serverDb");
    Mapper<User> mp(db);
    auto condition = (Criteria(User::Cols::_username, CompareOperator::EQ, account) ||
                     Criteria(User::Cols::_phone, CompareOperator::EQ, account) ||
                     Criteria(User::Cols::_email, CompareOperator::EQ, account));

    mp.findOne(condition,
        [callback, password, db, req, jwtSecret, cacheKey](User user) {
            if (!PasswordHelper::validatePassword(password, user.getValueOfPasswordHash()))
            {
                Json::Value ret;
                ret["error"] = "Invalid account or password";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k401Unauthorized);
                callback(resp);
                return;
            }

            auto token = jwt::create()
                .set_issuer("wechat_server")
                .set_payload_claim("user_id", jwt::claim(std::to_string(user.getValueOfId())))
                .set_expires_at(std::chrono::system_clock::now() + std::chrono::hours(24))
                .sign(jwt::algorithm::hs256{jwtSecret});

            // 写 Redis 缓存
            std::string cacheVal = std::to_string(user.getValueOfId()) + "|"
                + user.getValueOfNickname() + "|"
                + user.getValueOfAvatarUrl();
            RedisCache::instance().setexAsync(cacheKey, CACHE_TTL, cacheVal);

            user.setLastLoginTime(trantor::Date::date());
            user.setLastLoginIp(req->getPeerAddr().toIp());
            Mapper<User>(db).update(user,
                [token, user, callback](bool) {
                    Json::Value ret;
                    ret["token"] = token;
                    ret["user_id"] = (Json::Int64)user.getValueOfId();
                    ret["nickname"] = user.getValueOfNickname();
                    ret["avatar"] = user.getValueOfAvatarUrl();
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    callback(resp);
                },
                [callback, token](const DrogonDbException &e) {
                    LOG_ERROR << "Update login time failed: " << e.base().what();
                    Json::Value ret;
                    ret["token"] = token;
                    ret["error"] = "Login succeeded but failed to update login info";
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    callback(resp);
                }
            );
        },
        [callback](const DrogonDbException &e) {
            Json::Value ret;
            ret["error"] = "Invalid account or password";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k401Unauthorized);
            callback(resp);
        }
    );
}