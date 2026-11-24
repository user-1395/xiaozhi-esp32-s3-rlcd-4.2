#include "sdcard_service.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

static const char* TAG = "SdcardService";

// 挂载点与引脚：对齐微雪官方 06_SD_Card 例程
static const char* SD_MOUNT_POINT = "/sdcard";
static const int SD_PIN_CLK = 38;
static const int SD_PIN_CMD = 21;
static const int SD_PIN_D0  = 39;

SdcardService& SdcardService::GetInstance() {
    static SdcardService instance;
    return instance;
}

bool SdcardService::Init() {
    if (card_ != nullptr) {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 8;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20MHz 起步，稳

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit 模式：只用 CMD/CLK/D0
    slot_config.clk = (gpio_num_t)SD_PIN_CLK;
    slot_config.cmd = (gpio_num_t)SD_PIN_CMD;
    slot_config.d0  = (gpio_num_t)SD_PIN_D0;
    // 1-bit 模式下不用的数据线必须置为 -1，否则驱动会误占用 GPIO
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "SD 卡挂载失败：未插卡或格式非 FAT32（err=0x%x）", ret);
        } else {
            ESP_LOGW(TAG, "SD 卡挂载失败：err=0x%x（%s）", ret, esp_err_to_name(ret));
        }
        card_ = nullptr;
        return false;
    }

    card_ = card;
    ESP_LOGI(TAG, "SD 卡挂载成功 -> %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);
    return true;
}

bool SdcardService::SafePath(const std::string& path) const {
    // 必须以 /sdcard/ 开头，且不能含 ".." 防止路径穿越
    if (path.rfind("/sdcard/", 0) != 0) {
        return false;
    }
    if (path.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

int SdcardService::WriteFile(const std::string& path, const void* data, size_t len) {
    if (card_ == nullptr || !SafePath(path)) {
        return -1;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "写文件失败，无法打开: %s", path.c_str());
        return -2;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "写文件不完整 (%zu/%zu): %s", written, len, path.c_str());
        return -3;
    }
    return 0;
}

int SdcardService::AppendFile(const std::string& path, const void* data, size_t len) {
    if (card_ == nullptr || !SafePath(path)) {
        return -1;
    }
    FILE* f = fopen(path.c_str(), "ab");
    if (f == nullptr) {
        return -2;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (int)written;
}

int SdcardService::ReadFile(const std::string& path, std::string& out) {
    if (card_ == nullptr || !SafePath(path)) {
        return -1;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return -2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return -3;
    }
    out.resize((size_t)size);
    size_t read = (size_t)size > 0 ? fread(&out[0], 1, (size_t)size, f) : 0;
    fclose(f);
    out.resize(read);
    return (int)read;
}

int SdcardService::ListDir(const std::string& path, std::vector<std::string>& out) {
    if (card_ == nullptr || !SafePath(path)) {
        return -1;
    }
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return -2;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        // 判断是否为目录
        std::string full = path;
        if (!full.empty() && full.back() != '/') full += '/';
        full += ent->d_name;
        struct stat st;
        bool is_dir = (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        out.push_back(std::string(ent->d_name) + (is_dir ? "/" : ""));
    }
    closedir(dir);
    return 0;
}

int SdcardService::RemoveFile(const std::string& path) {
    if (card_ == nullptr || !SafePath(path)) {
        return -1;
    }
    return remove(path.c_str()) == 0 ? 0 : -2;
}

std::string SdcardService::CardInfo() {
    if (card_ == nullptr) {
        return "not mounted";
    }
    sdmmc_card_t* card = (sdmmc_card_t*)card_;
    char buf[160];
    // 只用稳定公开字段，避免不同 IDF 版本结构差异导致编译失败。
    // csd.capacity 是 512 字节块数，各 IDF 版本都存在。
    unsigned long long capacity_mb =
        (unsigned long long)card->csd.capacity / 2048ULL;  // blocks -> MB
    snprintf(buf, sizeof(buf),
             "name=%s capacity=%lluMB speed=%dMHz",
             card->cid.name,
             capacity_mb,
             card->max_freq_khz / 1000);
    return std::string(buf);
}
