#pragma once
#include <drogon/HttpController.h>

namespace api::v1
{
    class LoginController : public drogon::HttpController<LoginController>
    {
    public:
        METHOD_LIST_BEGIN
            ADD_METHOD_TO(LoginController::login, "/api/v1/login", drogon::Post);
        METHOD_LIST_END

        void login(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    };
}