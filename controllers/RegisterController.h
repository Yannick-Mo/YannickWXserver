#pragma once
#include <drogon/HttpController.h>

namespace api::v1
{
    class RegisterController : public drogon::HttpController<RegisterController>
    {
    public:
        METHOD_LIST_BEGIN
            ADD_METHOD_TO(RegisterController::reg, "/api/v1/register", drogon::Post);
        METHOD_LIST_END

        void reg(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    };
}