#include "LoginController.h"
#include <drogon/orm/DbClient.h>
#include <jwt-cpp/jwt.h>
#include <models/User.h>
#include "utils/PasswordHelper.h"   
#include <drogon/orm/Criteria.h>

using namespace drogon;
using namespace drogon::orm;
using namespace api::v1;
using namespace drogon_model::yaroserverdb;

// JWT 密钥（应从环境变量读取，此处仅示例）
const std::string JWT_SECRET = "your-256-bit-secret";

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

    std::string account = (*json)["account"].asString();  // 支持用户名/手机/邮箱
    std::string password = (*json)["password"].asString();

    auto db = app().getDbClient("serverDb");

    Mapper<User> mp(db);
    auto condition = (Criteria(User::Cols::_username, CompareOperator::EQ, account) ||
                     Criteria(User::Cols::_phone, CompareOperator::EQ, account) ||
                     Criteria(User::Cols::_email, CompareOperator::EQ, account));

    mp.findOne(condition,
        [callback, password, db, req](User user) {
            // 使用 PasswordHelper 验证密码
            if (!PasswordHelper::validatePassword(password, user.getValueOfPasswordHash()))
            {
                Json::Value ret;
                ret["error"] = "Invalid account or password";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k401Unauthorized);
                callback(resp);
                return;
            }

            // 生成 JWT
            auto token = jwt::create()
                .set_issuer("wechat_server")
                .set_payload_claim("user_id", jwt::claim(std::to_string(user.getValueOfId())))
                .set_expires_at(std::chrono::system_clock::now() + std::chrono::hours(24))
                .sign(jwt::algorithm::hs256{JWT_SECRET});

            // 更新最后登录时间
            user.setLastLoginTime(trantor::Date::date());
            user.setLastLoginIp(req->getPeerAddr().toIp());
            Mapper<User>(db).update(user,
                [token, user, callback](bool) {
                    Json::Value ret;
                    ret["token"] = token;
                    ret["user_id"] = user.getValueOfId();
                    ret["nickname"] = user.getValueOfNickname();
                    ret["avatar"] = user.getValueOfAvatarUrl();
                    auto resp = HttpResponse::newHttpJsonResponse(ret);
                    callback(resp);
                },
                [callback, token](const DrogonDbException &e) {
                    LOG_ERROR << "Update login time failed: " << e.base().what();
                    // 即使更新失败，登录仍然成功，返回 token
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