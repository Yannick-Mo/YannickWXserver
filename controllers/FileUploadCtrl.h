#pragma once
#include <drogon/HttpSimpleController.h>
#include <unordered_set>

namespace api::v1 {
class FileUploadCtrl : public drogon::HttpSimpleController<FileUploadCtrl> {
public:
    FileUploadCtrl();
    virtual void asyncHandleHttpRequest(const drogon::HttpRequestPtr& req,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) override;

    PATH_LIST_BEGIN
    PATH_ADD("/api/v1/upload/chunk", drogon::Get);      // 查询分片状态
    PATH_ADD("/api/v1/upload/chunk", drogon::Post);     // 上传分片
    PATH_ADD("/api/v1/upload/complete", drogon::Post);  // 合并分片
    PATH_ADD("/api/v1/upload/thumbnail", drogon::Post); // 上传缩略图
    PATH_LIST_END

private:
    std::string finalDir_;
    std::string chunkBaseDir_;
    size_t maxChunkSize_ = 10 * 1024 * 1024;
    int maxRetries_ = 5;
    std::unordered_set<std::string> allowedExtensions_;

    bool isAllowedExtension(const std::string& ext) const;
    std::string sanitizeExtension(const std::string& ext) const;
    std::string generateUniqueFilename(const std::string& ext, const std::string& dir) const;
    bool ensureDir(const std::string& dir) const;
    std::string getChunkDir(const std::string& fileHash) const;
    std::string getChunkPath(const std::string& fileHash, int chunkIndex) const;
    void saveChunkAsync(const std::string& path, std::shared_ptr<std::vector<char>> data,
                        std::function<void(bool)>&& callback) const;
    void mergeChunks(const std::string& fileHash, int totalChunks,
                     const std::string& finalFilename,
                     std::function<void(bool, const std::string&)>&& callback) const;
    void saveThumbnailAsync(const std::string& path, std::shared_ptr<std::vector<char>> data,
                            std::function<void(bool)>&& callback) const;
};
} // namespace api::v1