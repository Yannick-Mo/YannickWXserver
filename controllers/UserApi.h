#pragma once
#include <drogon/HttpController.h>

class UserApi : public drogon::HttpController<UserApi> {
public:
    METHOD_LIST_BEGIN
        // 在方法后添加过滤器名称 "JwtFilter"
        ADD_METHOD_TO(UserApi::getProfile, "/api/user/profile", drogon::Get, "JwtFilter");
        ADD_METHOD_TO(UserApi::updateProfile, "/api/user/profile", drogon::Post, "JwtFilter");
        ADD_METHOD_TO(UserApi::getAllGroupMembers, "/api/user/groupsAddMembers", drogon::Get, "JwtFilter");
        ADD_METHOD_TO(UserApi::searchUsers, "/api/user/search", drogon::Get, "JwtFilter");
    METHOD_LIST_END

    void getProfile(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void getAllGroupMembers(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void updateProfile(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void searchUsers(const drogon::HttpRequestPtr &req, 
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback);

};