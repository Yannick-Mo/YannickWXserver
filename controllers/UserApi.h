#pragma once
#include <drogon/HttpController.h>

class UserApi : public drogon::HttpController<UserApi> {
public:
    METHOD_LIST_BEGIN
        // 在方法后添加过滤器名称 "JwtFilter"
        ADD_METHOD_TO(UserApi::getProfile, "/api/user/profile", drogon::Get, "JwtFilter");
        ADD_METHOD_TO(UserApi::updateProfile, "/api/user/profile", drogon::Post, "JwtFilter");
    METHOD_LIST_END

    void getProfile(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void updateProfile(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};