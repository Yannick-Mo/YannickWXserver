#include "PasswordHelper.h"
#include <crypt.h>
#include <random>
#include <string>
#include <cstring>

std::string PasswordHelper::generateHash(const std::string &password)
{
    // 生成 16 字节随机 salt（bcrypt 需要 22 个字符的 base64 salt）
    static const char salt_chars[] =
        "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(salt_chars) - 2);

    char salt[30]; // 足够存储 "$2y$10$" + 22 个字符 + 终止符
    salt[0] = '$';
    salt[1] = '2';
    salt[2] = 'y';  // 使用 bcrypt（$2y$ 前缀）
    salt[3] = '$';
    salt[4] = '1';
    salt[5] = '0';  // 加密轮数 10（可根据需要调整）
    salt[6] = '$';
    for (int i = 0; i < 22; ++i) {
        salt[7 + i] = salt_chars[dis(gen)];
    }
    salt[29] = '\0';

    struct crypt_data data;
    data.initialized = 0;
    char *hash = crypt_r(password.c_str(), salt, &data);
    if (hash == nullptr) {
        return "";
    }
    return std::string(hash);
}

bool PasswordHelper::validatePassword(const std::string &password, const std::string &hash)
{
    struct crypt_data data;
    data.initialized = 0;
    char *newHash = crypt_r(password.c_str(), hash.c_str(), &data);
    if (newHash == nullptr) {
        return false;
    }
    return hash == newHash;
}