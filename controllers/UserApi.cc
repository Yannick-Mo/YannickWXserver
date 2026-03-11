#include "UserApi.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpRequest.h>
#include <json/json.h>

using namespace drogon;

void UserApi::getProfile(const HttpRequestPtr &req,
                         std::function<void(const HttpResponsePtr &)> &&callback) {
    // 使用 getAttributes()->get<T>(key)
    auto userId = req->getAttributes()->get<std::string>("user_id");

    Json::Value ret;
    ret["user_id"] = userId;
    ret["name"] = "Demo User";
    ret["email"] = userId + "@example.com";

    auto resp = HttpResponse::newHttpJsonResponse(ret);
    callback(resp);
}

void UserApi::updateProfile(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback) {
    auto userId = req->getAttributes()->get<std::string>("user_id");
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setBody("Invalid JSON");
        callback(resp);
        return;
    }

    LOG_INFO << "User " << userId << " updated profile: " << json->toStyledString();

    Json::Value ret;
    ret["status"] = "ok";
    ret["user_id"] = userId;
    auto resp = HttpResponse::newHttpJsonResponse(ret);
    callback(resp);
}