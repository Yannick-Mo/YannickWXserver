#pragma once
#include <string>

class PasswordHelper
{
public:
    // 生成 bcrypt 哈希（使用系统 crypt_r）
    static std::string generateHash(const std::string &password);

    // 验证密码与哈希是否匹配
    static bool validatePassword(const std::string &password, const std::string &hash);
};