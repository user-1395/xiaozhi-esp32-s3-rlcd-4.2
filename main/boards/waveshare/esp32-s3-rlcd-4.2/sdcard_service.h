#pragma once

// SD 卡服务：基于 ESP-IDF SDMMC 1-bit 模式挂载 /sdcard，
// 提供文件读写/列目录能力，供 HTTP 端点与后续离线 RAG/音乐/看图使用。
// 引脚对齐微雪官方 06_SD_Card 例程：CLK=GPIO38, CMD=GPIO21, D0=GPIO39。

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>

class SdcardService {
public:
    static SdcardService& GetInstance();

    // 挂载 SD 卡。非致命：失败只返回 false 并打印告警，不影响主功能。
    bool Init();
    bool IsReady() const { return card_ != nullptr; }

    // 写文件（覆写）。返回 0 成功，负数为 ESP 错误码。
    int WriteFile(const std::string& path, const void* data, size_t len);
    // 追加写。返回实际写入字节数。
    int AppendFile(const std::string& path, const void* data, size_t len);
    // 读整个文件到 buf。成功返回实际字节数，失败返回负值。
    int ReadFile(const std::string& path, std::string& out);
    // 列目录，返回条目名（含 / 后缀表示目录）。成功返回 0。
    int ListDir(const std::string& path, std::vector<std::string>& out);
    // 删除文件。成功返回 0。
    int RemoveFile(const std::string& path);

    // 卡信息（容量等），用于 /sd/status
    std::string CardInfo();

private:
    SdcardService() = default;
    ~SdcardService() = default;
    SdcardService(const SdcardService&) = delete;
    SdcardService& operator=(const SdcardService&) = delete;

    // 校验路径必须以 /sdcard/ 开头，防止越界访问
    bool SafePath(const std::string& path) const;

    void* card_ = nullptr;  // sdmmc_card_t*
};
