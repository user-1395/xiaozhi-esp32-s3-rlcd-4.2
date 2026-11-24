#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <string>
#include <mutex>
#include <soc/rtc.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "settings.h"
#include <cJSON.h>
#include "lvgl.h"
#include "managers/sensor_manager.h"
#include "managers/power_manager.h"
#include "ui_assets.h"
#include "managers/weather_manager.h"
#include "managers/todo_mcp_client.h"
#include "managers/local_mcp_bridge.h"
#include "screenshot_server.h"
#include "sdcard_service.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "waveshare_rlcd_4_2"

// 天气拉取任务：优先和风天气（实时/空气/预报/预警），失败回退免密钥源
// 成功 15 分钟更新一次（和风实时数据 10 分钟一更新、空气质量 1 小时一更新）；
// 失败按 60s -> 120s -> 240s -> 300s 退避重试，成功后退避清零。
static void WeatherFetchTask(void* arg) {
    vTaskDelay(pdMS_TO_TICKS(15000));
    uint32_t fail_backoff_ms = 60000;
    for (;;) {
        bool ok = WeatherManager::getInstance().update();
        uint32_t interval_ms = 0;
        if (ok) {
            fail_backoff_ms = 60000;
            interval_ms = 15u * 60u * 1000u;
        } else {
            interval_ms = fail_backoff_ms;
            fail_backoff_ms = (fail_backoff_ms >= 300000u) ? 300000u : (fail_backoff_ms * 2u);
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

// 电池状态快照：百分比 / 电压 / 充电判断 / 剩余电量与可续航估算
struct BatteryStatus {
    int percent = 0;          // 0-100
    uint16_t voltage_mv = 0;  // 电池端电压 mV
    bool known = false;       // 是否已能判定充电/放电（否则 charging/discharging 都为 false）
    bool charging = false;    // 是否外接电源（充电中或已充满）
    bool full = false;        // 是否已充满（充电且到达满电平台）
    bool discharging = false; // 是否纯电池供电（仅 known 时有意义）
    bool has_capacity = false;// 是否已知电池容量（否则不输出 mAh/续航，避免编造）
    int remaining_mah = 0;    // 估算剩余容量
    int runtime_min = 0;      // 估算可续航分钟
};

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;  // GPIO18 用户按键
    CustomLcdDisplay *display_ = nullptr;
    std::mutex battery_mutex_;   // 保护 GetBatteryStatusLocked 内的 static 采样历史/状态

    // 校验时间标签是否为 HH:MM（24 小时制）
    bool IsValidMemoTimeLabel(const std::string& time_str) {
        if (time_str.empty()) {
            return true;  // 允许无时间备忘
        }
        if (time_str.size() != 5 || time_str[2] != ':') {
            return false;
        }
        if (time_str[0] < '0' || time_str[0] > '9' ||
            time_str[1] < '0' || time_str[1] > '9' ||
            time_str[3] < '0' || time_str[3] > '9' ||
            time_str[4] < '0' || time_str[4] > '9') {
            return false;
        }
        int hh = (time_str[0] - '0') * 10 + (time_str[1] - '0');
        int mm = (time_str[3] - '0') * 10 + (time_str[4] - '0');
        return (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59);
    }

    // ===== 备忘录 NVS 读写辅助（P2 #11）=====
    // 读 NVS "memo"/"items" 并解析为 cJSON 数组；非数组或解析失败返回 nullptr。
    // 调用方负责 cJSON_Delete。
    cJSON* LoadMemoArray() {
        std::string json_str;
        {
            Settings settings("memo", false);
            json_str = settings.GetString("items", "[]");
        }
        cJSON *arr = cJSON_Parse(json_str.c_str());
        if (arr && !cJSON_IsArray(arr)) {
            cJSON_Delete(arr);
            arr = nullptr;
        }
        return arr;
    }

    // 将 cJSON 数组写回 NVS "memo"/"items"；arr 为空时不写入。
    void SaveMemoArray(cJSON* arr) {
        if (!arr) return;
        char *new_json = cJSON_PrintUnformatted(arr);
        if (!new_json) return;
        {
            Settings settings("memo", true);
            settings.SetString("items", new_json);
        }
        cJSON_free(new_json);
    }

    // 采集并格式化系统信息（P2 #5）：ShowSystemInfo 与 self.system.info 共用。
    // natural_language=true 返回适合 AI 朗读的自然语言；false 返回屏幕显示的多行块。
    std::string BuildSystemInfoString(bool natural_language) {
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

        rtc_cpu_freq_config_t cpu_freq_conf;
        rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
        uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;

        BatteryStatus bat = GetBatteryStatus();
        char battery_detail[128];
        if (bat.has_capacity) {
            if (natural_language) {
                snprintf(battery_detail, sizeof(battery_detail),
                         "电池电量%d%%，电压%.2fV，当前%s，估算剩余%dmAh、可续航约%d小时%d分钟",
                         bat.percent, bat.voltage_mv / 1000.0f,
                         (!bat.known ? "状态待判定" : (bat.charging ? (bat.full ? "已充满" : "正在充电") : "使用电池供电")),
                         bat.remaining_mah, bat.runtime_min / 60, bat.runtime_min % 60);
            } else {
                snprintf(battery_detail, sizeof(battery_detail), "%d%% %s %.2fV 剩余%dmAh 约%dh%02dmin",
                         bat.percent, (!bat.known ? "状态待判定" : (bat.charging ? (bat.full ? "已充满" : "充电中") : "放电中")),
                         bat.voltage_mv / 1000.0f, bat.remaining_mah,
                         bat.runtime_min / 60, bat.runtime_min % 60);
            }
        } else {
            if (natural_language) {
                snprintf(battery_detail, sizeof(battery_detail), "电池电量%d%%，电压%.2fV，当前%s",
                         bat.percent, bat.voltage_mv / 1000.0f,
                         (!bat.known ? "状态待判定" : (bat.charging ? (bat.full ? "已充满" : "正在充电") : "使用电池供电")));
            } else {
                snprintf(battery_detail, sizeof(battery_detail), "%d%% %s %.2fV",
                         bat.percent, (!bat.known ? "状态待判定" : (bat.charging ? (bat.full ? "已充满" : "充电中") : "放电中")),
                         bat.voltage_mv / 1000.0f);
            }
        }

        auto& app = Application::GetInstance();
        const char* wifi_status = "未连接";
        if (app.GetDeviceState() != kDeviceStateStarting &&
            app.GetDeviceState() != kDeviceStateWifiConfiguring) {
            wifi_status = "已连接";
        }

        uint64_t uptime_sec = esp_timer_get_time() / 1000000;
        uint32_t uptime_hours = uptime_sec / 3600;
        uint32_t uptime_mins = (uptime_sec % 3600) / 60;

        int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
        int psram_percent = total_psram > 0 ?
                            (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;

        char info[512];
        if (natural_language) {
            snprintf(info, sizeof(info),
                     "系统运行正常。CPU频率%luMHz，已运行%lu小时%lu分钟。"
                     "内存方面，SRAM使用了%dKB，占总量%dKB的%d%%；"
                     "PSRAM使用了%dMB，占总量%dMB的%d%%。"
                     "%s。WiFi%s。",
                     cpu_freq_mhz, uptime_hours, uptime_mins,
                     (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                     (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                     battery_detail, wifi_status);
        } else {
            snprintf(info, sizeof(info),
                     "=== 系统信息 ===\n"
                     "CPU: %luMHz\n"
                     "运行: %luh%lumin\n"
                     "SRAM: \n %dKB/%dKB (%d%%)\n"
                     "PSRAM: \n %dMB/%dMB (%d%%)\n"
                     "电池: %s\n"
                     "WiFi: %s\n"
                     "==============\n"
                     "\n",
                     cpu_freq_mhz,
                     uptime_hours, uptime_mins,
                     (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                     (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                     battery_detail, wifi_status);
        }
        return std::string(info);
    }

    void InitializeI2c() {
        // I2C 总线初始化
        // 这条 I2C 总线被多个设备共享：
        // - ES8311 音频解码器
        // - ES7210 音频编码器
        // - SHTC3 温湿度传感器
        // - PCF85063 RTC 时钟
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSensors() {
        // 初始化传感器（使用同一条 I2C 总线）
        SensorManager::getInstance().init(i2c_bus_);
        ESP_LOGI(TAG, "传感器初始化完成");
    }

    void InitializeButtons() { 
        // BOOT 按钮（GPIO0）- 主要交互按键
        boot_button_.OnClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // USER 按钮（GPIO18）- 辅助功能按键
        user_button_.OnClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            if (display_) {
                display_->CycleDisplayMode();
            }
            ESP_LOGI(TAG, "USER 按钮单击：切换天气页/音乐页");
        });

        user_button_.OnDoubleClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            // 双击：刷新所有数据（天气、传感器、时间）
            RefreshAllData();
        });

        user_button_.OnLongPress([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            // 长按：显示系统信息
            ShowSystemInfo();
        });
    }

    // USER 按钮功能实现
    void ShowSystemInfo() {
        // 使用统一构建函数（P2 #5）
        std::string info = BuildSystemInfoString(false);
        
        if (display_) {
            lv_obj_t* chat_label = display_->GetChatStatusLabel();
            if (chat_label) {
                // 暂停 DataUpdateTask 对 UI 的更新（避免锁竞争导致 watchdog 超时）
                display_->SetShowingSystemInfo(true);
                
                {
                    DisplayLockGuard lock(display_);
                    
                    // 先删除旧动画（防止冲突）
                    lv_anim_delete(chat_label, nullptr);
                    
                    // 🐟 鱼咬尾：拼接两份相同内容
                    std::string info_double = std::string(info) + std::string(info);
                    
                    // 🔑 关键修复：切换到 TOP_LEFT 绝对定位
                    // 原因：label 初始化时用的是 LV_ALIGN_LEFT_MID（居中对齐），
                    // LVGL 内部会存储这个对齐方式，布局刷新时会重新计算位置，
                    // 导致动画里 set_y 设的值被覆盖。
                    // 切换到 TOP_LEFT 后，Y=0 就是父容器顶部，动画不会被干扰。
                    const int text_x = 64 + 20;  // emotion_w + 间距，保持文字在分隔线右侧
                    lv_obj_align(chat_label, LV_ALIGN_TOP_LEFT, text_x, 0);
                    
                    lv_label_set_text(chat_label, info_double.c_str());
                    lv_label_set_long_mode(chat_label, LV_LABEL_LONG_WRAP);
                    
                    // 强制计算布局，获取实际高度
                    lv_obj_update_layout(chat_label);
                    int label_h = lv_obj_get_height(chat_label);  // 双份内容的总高度
                    int single_h = label_h / 2;  // 单份内容高度
                    
                    // 🐟 鱼咬尾动画原理：
                    // 内容 = [A][A]（两份完全相同的文字首尾相接）
                    // Y=0 时显示第一个 A 的开头
                    // 向上滚动到 Y=-single_h 时，显示第二个 A 的开头
                    // 因为两个 A 完全一样，动画重复跳回 Y=0 时视觉上无缝衔接！
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, chat_label);
                    lv_anim_set_values(&a, 0, -single_h);
                    lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒，让用户先看到开头
                    lv_anim_set_duration(&a, single_h * 30);  // 速度：每像素 30ms
                    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_set_repeat_delay(&a, 0);  // 无缝重复，不停顿
                    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
                        lv_obj_set_y((lv_obj_t *)obj, v);
                    });
                    lv_anim_start(&a);
                }  // ← DisplayLockGuard 在这里自动释放
            }
        }
        
        ESP_LOGI(TAG, "系统信息已显示");
    }

    void RefreshAllData() {
        ESP_LOGI(TAG, "手动刷新所有数据...");
        
        // 暂时停用板载和风天气拉取，天气请通过 MCP 工具 self.weather.update 刷新
        // WeatherManager::getInstance().update();
        
        // 重新同步 NTP 时间
        SensorManager::getInstance().syncNtpTime();
        
        // 强制刷新屏幕显示
        if (display_) {
            display_->SetChatMessage("system", "正在刷新数据...\n时间已更新，天气等待 MCP 同步");
        }
        
        ESP_LOGI(TAG, "数据刷新完成");
    }

    void RegisterSystemTools() {
        auto& mcp_server = McpServer::GetInstance();
        // ===== 系统信息工具 =====
        mcp_server.AddTool("self.system.info",
            "Get device system information (CPU, memory, battery, WiFi status).\n"
            "Use when user asks: '系统信息', 'CPU频率', '内存使用情况', '电量多少', 'system status', 'how much RAM'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                ESP_LOGI(TAG, "AI查询系统信息");
                // 系统信息统一由 BuildSystemInfoString 构建（P2 #5）
                return BuildSystemInfoString(true);
            });

    }

    void RegisterWeatherTools() {
        auto& mcp_server = McpServer::GetInstance();
        // ===== 天气写入工具（由 AI 侧 MCP 查询后回写到设备）=====
        mcp_server.AddTool("self.weather.update",
            "Write weather data to the device screen cache.\n"
            "Use this after AI gets weather from an external MCP/weather source.\n"
            "Args:\n"
            "  `city`: City name (e.g. '苏州')\n"
            "  `text`: Weather text (e.g. '晴', '多云', '小雨')\n"
            "  `temp`: Temperature string without unit (e.g. '5', '-2', '26')\n"
            "  `update_time`: Optional time text (e.g. '2026-02-11 23:45')",
            PropertyList({
                Property("city", kPropertyTypeString),
                Property("text", kPropertyTypeString),
                Property("temp", kPropertyTypeString),
                Property("update_time", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto city = properties["city"].value<std::string>();
                auto text = properties["text"].value<std::string>();
                auto temp = properties["temp"].value<std::string>();
                auto update_time = properties["update_time"].value<std::string>();

                bool ok = WeatherManager::getInstance().updateFromExternal(city, text, temp, update_time);
                if (!ok) {
                    return std::string("天气写入失败：请检查 city/text/temp 是否为空");
                }

                ESP_LOGI(TAG, "AI写入天气成功: %s %s %s°C", city.c_str(), text.c_str(), temp.c_str());
                return std::string("天气已更新：") + city + " " + text + " " + temp + "°C";
            });
        
        // ===== 天气城市设置（语音可调用，覆盖 IP 定位，持久化到 NVS）=====
        mcp_server.AddTool(
            "self.weather.set_city",
            "Set the city used for weather queries (overrides IP-based location, persisted in NVS).\n"
            "Use when user says: 天气城市改成南京, set weather city.\n"
            "Args:\n"
            "  `city`: city name, e.g. 南京\n"
            "  `province`: optional province/municipality to disambiguate duplicate city names (e.g. 江苏 for 常州), can be empty",
            PropertyList({
                Property("city", kPropertyTypeString, std::string("")),
                Property("province", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto city = properties["city"].value<std::string>();
                auto province = properties["province"].value<std::string>();
                if (city.empty()) {
                    return std::string("请提供城市名，例如：南京");
                }
                WeatherManager::getInstance().SetCity(city, province);
                return std::string("天气城市已设置为 ") + city +
                       (province.empty() ? std::string("") : ("（" + province + "）")) +
                       "，稍后自动刷新";
            }
        );

        // ===== 天气城市查询（诊断：当前城市 / 来源 / LocationID）=====
        mcp_server.AddTool(
            "self.weather.get_city",
            "Report the city currently used for weather and where it came from.\n"
            "Use when user asks: 天气城市是哪里, which city is the weather for",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                auto& wm = WeatherManager::getInstance();
                WeatherData data = wm.getLatestData();
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "天气城市：%s（来源：%s，LocationID：%s），最新显示：%s %s %s°C",
                         wm.city().c_str(), wm.citySource().c_str(),
                         wm.locationId().empty() ? "(未解析)" : wm.locationId().c_str(),
                         data.city.c_str(), data.text.c_str(), data.temp.c_str());
                return std::string(buf);
            }
        );

        // ===== 天气查询（语音问天气时切详情页并播报当日天气）=====
        mcp_server.AddTool(
            "self.weather.query",
            "Query today's weather, switch the screen to the weather detail page, and return a spoken summary.\n"
            "Use when user asks: 今天天气怎么样, 今天天气, 天气如何, weather today.\n"
            "The returned text is meant to be spoken to the user; the device returns to the home page after speaking.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                WeatherData d = WeatherManager::getInstance().getLatestData();
                if (!d.valid) {
                    return std::string("天气数据还没准备好，请稍后再问一次。");
                }
                std::string text = "今天" + d.city + "，" + d.text + "，当前" + d.temp + "°C";
                if (!d.feels_like.empty()) text += "，体感" + d.feels_like + "°C";
                if (!d.humidity.empty()) text += "，湿度" + d.humidity + "%";
                if (!d.wind_dir.empty()) text += "，" + d.wind_dir + d.wind_scale + "级";
                if (!d.aqi.empty()) {
                    text += "，空气质量" + d.aqi;
                    if (!d.aqi_category.empty()) text += " " + d.aqi_category;
                }
                if (d.daily_count > 0) {
                    text += "。今日" + d.daily[0].temp_min + "到" + d.daily[0].temp_max + "°C";
                }
                if (!d.sunrise.empty() && !d.sunset.empty()) {
                    text += "，日出" + d.sunrise + "，日落" + d.sunset;
                }
                text += d.warning.empty() ? "。暂无气象预警。" : ("。气象预警：" + d.warning + "。");

                if (display_) {
                    uint32_t est_ms = 6000u + static_cast<uint32_t>(text.size()) * 220u;
                    if (est_ms > 60000u) est_ms = 60000u;
                    display_->ShowWeatherDetailForQuery(est_ms);
                }
                ESP_LOGI(TAG, "语音天气查询: %s", text.c_str());
                return text;
            }
        );

    }

    // ===== 倒计时工具（固件级，独立于 MCP 备忘/待办）=====
    void RegisterCountdownTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool(
            "self.countdown.start",
            "Start a firmware countdown timer. Use when user says: 倒计时5分钟, 倒计时 1 分钟, start a countdown.\n"
            "The screen automatically switches to the dedicated black countdown page and shows remaining time in seconds.\n"
            "Args:\n  `seconds`: countdown length in seconds (1-86400)",
            PropertyList({Property("seconds", kPropertyTypeInteger, 1, 86400)}),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!display_) {
                    return std::string("显示器未初始化，暂时无法启动倒计时");
                }
                int seconds = properties["seconds"].value<int>();
                if (!display_->StartCountdown(seconds)) {
                    return std::string("倒计时启动失败：时长无效或并发倒计时过多");
                }
                char buf[64];
                snprintf(buf, sizeof(buf), "倒计时已开始：%d分%d秒", seconds / 60, seconds % 60);
                return std::string(buf);
            });

        mcp_server.AddTool(
            "self.countdown.pause",
            "Pause the running countdown. Use when user says: 暂停倒计时, 暂停闹钟, pause countdown.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!display_ || !display_->PauseCountdown()) {
                    return std::string("当前没有可暂停的倒计时");
                }
                return std::string("倒计时已暂停");
            });

        mcp_server.AddTool(
            "self.countdown.resume",
            "Resume the paused countdown. Use when user says: 继续倒计时, 继续闹钟, resume countdown.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!display_ || !display_->ResumeCountdown()) {
                    return std::string("当前没有可继续的倒计时");
                }
                return std::string("倒计时已继续");
            });

        mcp_server.AddTool(
            "self.countdown.cancel",
            "Cancel the running countdown. Use when user says: 取消倒计时, 取消闹钟, cancel countdown.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!display_ || !display_->CancelCountdown()) {
                    return std::string("当前没有可取消的倒计时");
                }
                return std::string("倒计时已取消");
            });

        mcp_server.AddTool(
            "self.countdown.status",
            "Report remaining countdown seconds. Use when user asks: 倒计时还有多久, 还剩多少时间.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!display_ || !display_->IsCountdownActive()) {
                    return std::string("当前没有进行中的倒计时");
                }
                char buf[64];
                snprintf(buf, sizeof(buf), "倒计时剩余 %d 秒", display_->GetCountdownRemainingSec());
                return std::string(buf);
            });
    }

    void RegisterDisplayTools() {
        auto& mcp_server = McpServer::GetInstance();
        // ===== 配网工具 =====
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });

        // ===== 屏幕切换工具（语音可调用）=====
        mcp_server.AddTool(
            "self.disp.switch",
            "Switch display page among weather, weather detail and music.\n"
            "Use when user says: '切到音乐页', '打开天气页', '看天气详情', '切换屏幕', 'switch screen'.\n"
            "Args:\n"
            "  `mode`: 'toggle' | 'weather' | 'detail' | 'music' (default: 'toggle')",
            PropertyList({
                Property("mode", kPropertyTypeString, std::string("toggle"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!display_) {
                    return std::string("显示器未初始化，暂时无法切换页面");
                }

                auto mode = properties["mode"].value<std::string>();

                // 统一小写判断
                for (auto& ch : mode) {
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = static_cast<char>(ch - 'A' + 'a');
                    }
                }

                display_->NotifyUserActivity();

                if (mode == "toggle") {
                    display_->CycleDisplayMode();
                } else if (mode == "music") {
                    display_->SwitchToMusicPage();
                } else if (mode == "weather") {
                    display_->SwitchToWeatherPage();
                } else if (mode == "detail") {
                    display_->SwitchToWeatherDetailPage();
                } else {
                    return std::string("参数 mode 无效，请使用 toggle/weather/detail/music");
                }

                if (display_->IsCountdownMode()) return std::string("倒计时进行中，已保持倒计时页");
                if (display_->IsMusicMode()) return std::string("已切换到音乐页");
                if (display_->IsDetailMode()) return std::string("已切换到天气详情页");
                return std::string("已切换到天气页");
            }
        );

    }

    void RegisterMemoTools() {
        auto& mcp_server = McpServer::GetInstance();
        // ===== 备忘录工具（多条列表模式）=====
        // NVS key "items" 存储 JSON 数组: [{"t":"15:00","c":"开会"}, ...]

        // 添加一条备忘
        mcp_server.AddTool("self.memo.add",
            "Add a memo / reminder / todo item. It will be persistently displayed on the device screen and survives reboot.\n"
            "Use when user says: '提醒我下午3点开会', '记住买牛奶', '待办写周报'\n"
            "Args:\n"
            "  `content`: Short memo text (max ~8 Chinese chars for best display on the small screen)\n"
            "  `time`: Time label in strict HH:MM 24-hour format (e.g. '07:30', '15:00'). Empty string if no specific time.\n"
            "Important:\n"
            "  - You MUST convert relative expressions to HH:MM before calling this tool.\n"
            "  - Examples: '5分钟后' -> '21:18', '半小时后' -> '21:43', '晚上8点' -> '20:00'.\n"
            "  - Do NOT pass natural language like '5分钟后' or '明天'.",
            PropertyList({
                Property("content", kPropertyTypeString),
                Property("time", kPropertyTypeString, std::string(""))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto content = properties["content"].value<std::string>();
                auto time_str = properties["time"].value<std::string>();
                if (!IsValidMemoTimeLabel(time_str)) {
                    return std::string("时间格式无效：请使用 HH:MM（24小时制），例如 07:30、15:00；不要传“5分钟后”这类自然语言");
                }

                // 读取现有列表
                cJSON *arr = LoadMemoArray();
                if (!arr) arr = cJSON_CreateArray();

                // 限制最多 10 条
                if (cJSON_GetArraySize(arr) >= 10) {
                    cJSON_Delete(arr);
                    return std::string("备忘已满（最多10条），请先完成或清除一些");
                }

                // 追加新条目
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "t", time_str.c_str());
                cJSON_AddStringToObject(item, "c", content.c_str());
                cJSON_AddItemToArray(arr, item);

                // 写回 NVS
                int count = cJSON_GetArraySize(arr);
                SaveMemoArray(arr);
                cJSON_Delete(arr);

                // 刷新屏幕
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "备忘已添加: 内容=%s, 时间=%s",
                         content.c_str(), time_str.c_str());
                return std::string("已添加备忘: ") + content + "（共" + std::to_string(count) + "条）";
            });

        // 查看所有备忘
        mcp_server.AddTool("self.memo.list",
            "List all memos / reminders / todos on the device.\n"
            "Use when user asks: '我有什么待办', '看看备忘', 'what do I need to do'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                cJSON *arr = LoadMemoArray();
                if (!arr || cJSON_GetArraySize(arr) == 0) {
                    if (arr) cJSON_Delete(arr);
                    return std::string("当前没有备忘");
                }

                std::string result = "当前备忘列表:\n";
                int count = cJSON_GetArraySize(arr);
                for (int i = 0; i < count; i++) {
                    cJSON *item = cJSON_GetArrayItem(arr, i);
                    cJSON *t = cJSON_GetObjectItem(item, "t");
                    cJSON *c = cJSON_GetObjectItem(item, "c");
                    result += std::to_string(i + 1) + ". ";
                    if (t && cJSON_IsString(t) && strlen(t->valuestring) > 0) {
                        result += "[";
                        result += t->valuestring;
                        result += "] ";
                    }
                    if (c && cJSON_IsString(c)) result += c->valuestring;
                    result += "\n";
                }
                cJSON_Delete(arr);
                return result;
            });

        // 完成/删除某条备忘（按序号）
        mcp_server.AddTool("self.memo.done",
            "Mark a memo as done and remove it from the list.\n"
            "Use when user says: '第一条做完了', '删掉买牛奶那条', '完成了开会'\n"
            "Args:\n"
            "  `index`: 1-based index of the memo to remove. If unsure, call self.memo.list first.",
            PropertyList({
                Property("index", kPropertyTypeInteger, 1, 10)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int idx = properties["index"].value<int>();

                cJSON *arr = LoadMemoArray();
                if (!arr) return std::string("备忘列表为空");

                int count = cJSON_GetArraySize(arr);
                if (idx < 1 || idx > count) {
                    cJSON_Delete(arr);
                    return std::string("序号无效，当前共") + std::to_string(count) + "条备忘";
                }

                // 获取被删除条目的内容用于反馈
                cJSON *removed = cJSON_GetArrayItem(arr, idx - 1);
                cJSON *c = cJSON_GetObjectItem(removed, "c");
                std::string removed_text = (c && c->valuestring) ? c->valuestring : "";

                cJSON_DeleteItemFromArray(arr, idx - 1);

                // 写回 NVS
                SaveMemoArray(arr);
                cJSON_Delete(arr);

                // 刷新屏幕
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "备忘已完成: %s", removed_text.c_str());
                return std::string("已完成: ") + removed_text;
            });

        // 清空所有备忘
        mcp_server.AddTool("self.memo.clear",
            "Clear ALL memos / reminders / todos.\n"
            "Use when user says: '清空备忘', '全部删掉', 'clear all memos'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                {
                    Settings settings("memo", true);
                    settings.EraseKey("items");
                }
                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "所有备忘已清除");
                return std::string("所有备忘已清除");
            });

    }

    void RegisterTodoTools() {
        auto& mcp_server = McpServer::GetInstance();
        // 待办同步：AI 侧先用自己的待办工具（如 list_todos）取到当日待办，再整批推给设备
        // 设备本身不是 MCP 客户端（无 ws 能力），因此由 AI 主动下发
        mcp_server.AddTool("self.todo.sync",
            "Replace the todo list shown on the device screen with today's todos.\n"
            "Call this AFTER fetching the user's todos from your own todo service (e.g. list_todos).\n"
            "Each array element is one todo row's content text; the device renders todos in a two-line\n"
            "time/content (t/c) layout, with the time line left empty for entries passed here.\n"
            "Args:\n"
            "  items: JSON array of strings. Pass [] to clear. Max 10 rows, keep each row short.",
            PropertyList({
                Property("items", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto items = properties["items"].value<std::string>();

                cJSON *src = cJSON_Parse(items.c_str());
                if (src == nullptr || !cJSON_IsArray(src)) {
                    if (src) cJSON_Delete(src);
                    return std::string("参数 items 必须是 JSON 字符串数组");
                }

                cJSON *arr = cJSON_CreateArray();
                int count = 0;
                cJSON *it = nullptr;
                cJSON_ArrayForEach(it, src) {
                    if (count >= 10) break;
                    if (!cJSON_IsString(it)) continue;
                    const char *text = it->valuestring;
                    if (text == nullptr || text[0] == 0) continue;
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "t", "");
                    cJSON_AddStringToObject(item, "c", text);
                    cJSON_AddItemToArray(arr, item);
                    count++;
                }
                cJSON_Delete(src);

                SaveMemoArray(arr);
                cJSON_Delete(arr);

                if (display_) display_->RefreshMemoDisplay();
                ESP_LOGI(TAG, "待办已同步: %d 条", count);
                return std::string("已同步 ") + std::to_string(count) + " 条待办到屏幕";
            });

        // 配置待办 MCP 接入点：设备会自己周期拉取 list_todos 并刷新待办卡
        mcp_server.AddTool("self.todo.set_url",
            "Configure the MCP endpoint the device uses to pull today's todos by itself.\n"
            "The device connects to this websocket JSON-RPC endpoint and calls list_todos.\n"
            "Use only when the user asks to change the todo service address.\n"
            "Args:\n"
            "  url: websocket URL starting with ws:// or wss://, usually ending with "
            "/mcp_endpoint/call/?token=...",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
                    return std::string("地址必须以 ws:// 或 wss:// 开头");
                }
                TodoMcpClient::GetInstance().SetUrl(url);
                TodoMcpClient::GetInstance().RequestRefresh();
                ESP_LOGI(TAG, "待办 MCP 地址已更新: %s", url.c_str());
                return std::string("待办 MCP 地址已保存，稍后自动拉取");
            });
    }

    // 路径 B：本地 MCP 桥接。云端 LLM 的工具调用经设备 HTTP POST 转发到
    // 局域网 PC 上的 Python 服务，实现语音生成 Excel / 写文件 / 查电脑状态。
    void RegisterLocalMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.local.call",
            "Call a tool on the user's local Windows PC to READ or WRITE files, list files, or check local machine status. "
            "Use when the user asks to GENERATE/SAVE a file ON THE LOCAL COMPUTER / PC / 电脑 / 本地 — including Excel "
            "spreadsheets (表格/Excel), PPT/presentations (PPT/幻灯片/演示文稿), Word documents (Word/文档/报告), "
            "PDF files, notes or text; OR asks to READ / VIEW / OPEN the content of a local file (“读一下刚才的文件/"
            "表格/PPT/文档里写了什么/查看文件内容”); OR asks about local disk space / computer status.\n"
            "ALSO use this tool for the LOCAL KNOWLEDGE BASE (知识库/RAG/资料库): when the user asks to look up / "
            "search knowledge (“查一下知识库/资料里有没有提到XX/文档里怎么说的/把这份文档加入知识库/知识库里有哪些文件”), "
            "or asks to add/remove/list knowledge-base documents. Route these to kb_search / kb_add / kb_list / kb_remove.\n"
            "Available tools and their args (pass as a JSON object string in `args`):\n"
            "- create_excel: {filename, title, headers:[str], rows:[[str]], sheet_name?} -> generate .xlsx\n"
            "- read_excel: {filename, max_rows?} -> READ an .xlsx into readable text (USE THIS for xlsx)\n"
            "- create_ppt: {filename, title, slides:[{title, bullets:[str]}]} -> generate .pptx (cover + slides)\n"
            "- read_ppt: {filename, max_slides?} -> READ a .pptx into readable text\n"
            "- create_word: {filename, title, paragraphs:[str], headings?:[str]} -> generate .docx\n"
            "- read_word: {filename, max_paragraphs?} -> READ a .docx into readable text\n"
            "- create_pdf: {filename, title, paragraphs:[str]} -> generate .pdf (supports Chinese)\n"
            "- read_pdf: {filename, max_pages?} -> READ a .pdf into readable text (supports Chinese)\n"
            "- write_note: {filename, content} -> write a .md/.txt note\n"
            "- read_text_file: {filename} -> read a plain text/.md file\n"
            "- list_generated_files: {} -> list files on the PC\n"
            "- kb_search: {query, top_k?} -> SEARCH the local knowledge base, returns relevant doc excerpts + sources\n"
            "- kb_add: {path?} -> add a document/folder to the knowledge base (incremental); empty path = add all docs in kb/docs\n"
            "- kb_list: {} -> list documents already in the knowledge base\n"
            "- kb_remove: {name} -> remove a document from the knowledge base index\n"
            "- kb_stats: {} -> knowledge base status (doc count, chunk count, directory)\n"
            "- list_tools: {} -> get the FULL, up-to-date tool list and usage\n"
            "- system_status: {} -> PC platform / CPU / free disk / time\n"
            "IMPORTANT: match the tool to the file type — use read_excel for xlsx, read_ppt for pptx, read_word for "
            "docx, read_pdf for pdf (read_text_file returns garbage for these binary formats). If unsure which tools "
            "exist or how to call one, call tool='list_tools' with args={} first — the list is dynamic and may include "
            "tools beyond those above.\n"
            "Args:\n"
            "  tool: one of the tool names above\n"
            "  args: JSON object string with that tool's parameters, e.g. "
            "{\"filename\":\"本周待办.xlsx\",\"title\":\"本周待办\",\"headers\":[\"时间\",\"事项\"],\"rows\":[[\"周一\",\"周会\"]]}",
            PropertyList({
                Property("tool", kPropertyTypeString),
                Property("args", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto tool = properties["tool"].value<std::string>();
                auto args = properties["args"].value<std::string>();
                std::string out;
                bool ok = LocalMcpBridge::GetInstance().Call(tool, args, out);
                if (!ok) return std::string("本地工具调用失败：") + out;
                return out;
            });

        mcp_server.AddTool("self.local.set_url",
            "Set the HTTP address of the local PC MCP bridge service. "
            "Use only when the user asks to change the local service address.\n"
            "Args:\n"
            "  url: http URL, e.g. http://192.168.1.11:8123",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                if (url.rfind("http://", 0) != 0) {
                    return std::string("地址必须以 http:// 开头");
                }
                LocalMcpBridge::GetInstance().SetUrl(url);
                return std::string("本地服务地址已保存：") + url;
            });
    }

    // P2 #6：按域拆分的工具注册入口；InitializeTools 仅负责依次调用。
    void InitializeTools() {
        RegisterSystemTools();
        RegisterWeatherTools();
        RegisterCountdownTools();
        RegisterDisplayTools();
        RegisterMemoTools();
        RegisterTodoTools();
        RegisterLocalMcpTools();
    }

    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH, RLCD_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, spi_config);
        
        // 启动天气站数据更新任务
        display_->StartDataUpdateTask();

        // 读取 NVS 里可运行时修改的天气配置（api_host / api_key / city）
        WeatherManager::getInstance().loadConfigFromNvs();

        // 启动天气网络拉取任务（独立任务，避免 HTTP 阻塞 LVGL 刷新）
        xTaskCreate(WeatherFetchTask, "weather_fetch", 8192, nullptr, 2, nullptr);

        // 启动待办 MCP 拉取任务：周期性调用外部 MCP 接入点的 list_todos，
        // 把当日待办写到右侧待办卡（独立任务 + 独立 websocket 连接）
        TodoMcpClient::GetInstance().Start([this]() {
            if (display_ != nullptr) {
                display_->RefreshMemoDisplay();
            }
        });
    }

    // 电压 → 电量百分比：单节锂电静置 OCV 曲线，分段线性插值（比原来的抛物线拟合更贴近实际）
    static int BatteryPercentFromMv(int mv) {
        static const int kTable[][2] = {
            {4200, 100}, {4150, 97}, {4100, 93}, {4050, 88}, {4000, 82},
            {3950, 74},  {3900, 65}, {3850, 56}, {3800, 46}, {3750, 35},
            {3700, 25},  {3650, 15}, {3600, 8},  {3500, 4},  {3400, 2}, {3300, 0},
        };
        const int n = (int)(sizeof(kTable) / sizeof(kTable[0]));
        if (mv >= kTable[0][0]) return 100;
        if (mv <= kTable[n - 1][0]) return 0;
        for (int i = 0; i < n - 1; i++) {
            int hi_mv = kTable[i][0], hi_pct = kTable[i][1];
            int lo_mv = kTable[i + 1][0], lo_pct = kTable[i + 1][1];
            if (mv <= hi_mv && mv >= lo_mv) {
                return lo_pct + (int)((long)(mv - lo_mv) * (hi_pct - lo_pct) / (hi_mv - lo_mv));
            }
        }
        return 0;
    }

    // --- 电池读取封装边界（P2 #7）---
    // BatteryGetVoltage 只做 ADC 采样（调用方须持 battery_mutex_）；
    // GetBatteryStatus() 是唯一对外线程安全快照入口，状态机/历史队列封装在其内部。
    // 电池电压（mV）：过采样 + 去极值平均，再按分压比还原到电池端
    uint16_t BatteryGetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            esp_err_t e_unit = adc_oneshot_new_unit(&init_config, &adc_handle);

            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            esp_err_t e_chan = (e_unit == ESP_OK)
                                   ? adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config)
                                   : e_unit;

            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            esp_err_t e_cali = (e_chan == ESP_OK)
                                   ? adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle)
                                   : e_chan;

            if (e_unit == ESP_OK && e_chan == ESP_OK && e_cali == ESP_OK) {
                initialized = true;
            } else {
                ESP_LOGE(TAG, "电池 ADC 初始化失败: unit=%s chan=%s cali=%s",
                         esp_err_to_name(e_unit), esp_err_to_name(e_chan), esp_err_to_name(e_cali));
                if (e_unit != ESP_OK && adc_handle) {
                    adc_oneshot_del_unit(adc_handle);
                    adc_handle = nullptr;
                }
            }
        }
        if (!initialized) {
            return 0;
        }
        int sum_mv = 0, cnt = 0;
        int min_mv = 0x7fffffff, max_mv = -1;
        for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
            int raw_value = 0, pin_mv = 0;
            if (adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value) != ESP_OK) continue;
            if (adc_cali_raw_to_voltage(cali_handle, raw_value, &pin_mv) != ESP_OK) continue;
            if (pin_mv < min_mv) min_mv = pin_mv;
            if (pin_mv > max_mv) max_mv = pin_mv;
            sum_mv += pin_mv;
            cnt++;
        }
        if (cnt == 0) {
            return 0;
        }
        // 去掉最大、最小各一个，抑制接触抖动/毛刺
        int avg_pin_mv = (cnt > 2) ? (sum_mv - min_mv - max_mv) / (cnt - 2) : sum_mv / cnt;
        int battery_mv = (int)((float)avg_pin_mv * BATTERY_ADC_DIVIDER + 0.5f);
        return (uint16_t)battery_mv;
    }

    // 采集一次电池状态：DataUpdateTask / 系统信息 / MCP 可能并发调用，
    // 内部 static 采样历史必须串行化，否则电量会因数据竞争跳变
    BatteryStatus GetBatteryStatus() {
        std::lock_guard<std::mutex> lock(battery_mutex_);
        return GetBatteryStatusLocked();
    }

    BatteryStatus GetBatteryStatusLocked() {
        BatteryStatus st;
        uint16_t mv = BatteryGetVoltage();
        st.voltage_mv = mv;

        // 充电芯片 STAT 未接 SoC：用电压趋势 + 满电平台判定；证据不足时保持「未知」，
        // 不硬猜成放电（否则开机瞬间会误报放电、并可能误触发低电量弹窗）。
        static uint32_t last_sample_ms = 0;
        static uint16_t hist_mv[60] = {0};        // 10 s/格 → 窗口约 10 分钟
        static int hist_count = 0, hist_pos = 0;
        static int charge_state = -1;             // -1=未知 0=放电 1=充电
        const uint32_t kSampleIntervalMs = 9000;  // 与主循环 10 s 对齐
        const int kTrendUpMv = 10, kTrendDownMv = 10;

        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (last_sample_ms == 0 || now_ms - last_sample_ms >= kSampleIntervalMs) {
            last_sample_ms = now_ms;
            hist_mv[hist_pos] = mv;
            hist_pos = (hist_pos + 1) % 60;
            if (hist_count < 60) hist_count++;
            if (hist_count >= 6) {
                int newest = (hist_pos + 60 - 1) % 60;
                int oldest = (hist_pos + 60 - hist_count) % 60;
                int delta_mv = (int)hist_mv[newest] - (int)hist_mv[oldest];
                if (delta_mv >= kTrendUpMv) charge_state = 1;
                else if (delta_mv <= -kTrendDownMv) charge_state = 0;
            }
        }
        // 没有趋势证据时，只有电压停在满电平台（CV 阶段，只有接了充电器才可能）才判充电
        if (charge_state < 0 && mv + 20 >= BATTERY_FULL_MV) charge_state = 1;
        // 采满 30 次（约 5 分钟）仍无趋势：纯电池放电时电压下降很慢、短窗口测不出，按放电处理
        if (charge_state < 0 && hist_count >= 30) charge_state = 0;

        st.known = (charge_state >= 0);
        st.charging = (charge_state == 1);
        st.discharging = (charge_state == 0);
        st.full = st.charging && (mv + 20 >= BATTERY_FULL_MV);

        // 放电时按负载压降补偿回静置电压，充电/未知时不补偿
        int comp_mv = st.discharging ? (int)BATTERY_LOAD_COMP_MV : 0;
        int pct = BatteryPercentFromMv((int)mv + comp_mv);
        // 1% 死区：读数抖动不来回跳数字
        static int last_pct = -1;
        if (last_pct >= 0) {
            int diff = (pct > last_pct) ? (pct - last_pct) : (last_pct - pct);
            if (diff <= 1) pct = last_pct;
        }
        last_pct = pct;
        st.percent = pct;

#if (BATTERY_CAPACITY_MAH > 0) && (BATTERY_AVG_CURRENT_MA > 0)
        st.has_capacity = true;
        st.remaining_mah = (int)((long)BATTERY_CAPACITY_MAH * pct / 100);
        st.runtime_min = st.remaining_mah * 60 / BATTERY_AVG_CURRENT_MA;
#endif
        return st;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), user_button_(USER_BUTTON_GPIO) {    
        InitializeI2c();
        InitializeSensors();  // 在 I2C 初始化后立即初始化传感器
        InitializeButtons();     
        InitializeTools();
        InitializeLcdDisplay();
        // 最后启用 DVFS + 自动 light sleep：此前各外设已初始化完毕
        PowerManager::GetInstance().Init();
    }

    // 应用层的省电等级 → 板级 DVFS 最高频率
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        WifiBoard::SetPowerSaveLevel(level);
        switch (level) {
            case PowerSaveLevel::PERFORMANCE:
                PowerManager::GetInstance().SetLevel(PowerManager::Level::kPerformance);
                break;
            case PowerSaveLevel::BALANCED:
                PowerManager::GetInstance().SetLevel(PowerManager::Level::kBalanced);
                break;
            case PowerSaveLevel::LOW_POWER:
            default:
                PowerManager::GetInstance().SetLevel(PowerManager::Level::kLowPower);
                break;
        }
    }

    // 记录 DHCP 下发的 DNS，便于排查无法解析域名的问题
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback([this, callback](NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::Connected) {
                // 网络刚连上：立刻拉一次待办
                TodoMcpClient::GetInstance().RequestRefresh();
                esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif != nullptr) {
                    esp_netif_dns_info_t info = {};
                    bool dns_missing = true;
                    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &info) == ESP_OK) {
                        ESP_LOGI(TAG, "STA main DNS: " IPSTR, IP2STR(&info.ip.u_addr.ip4));
                        dns_missing = (info.ip.u_addr.ip4.addr == 0);
                    } else {
                        ESP_LOGW(TAG, "STA main DNS: unavailable");
                    }
                    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &info) == ESP_OK) {
                        ESP_LOGI(TAG, "STA backup DNS: " IPSTR, IP2STR(&info.ip.u_addr.ip4));
                    }
                    // 部分热点（如 Windows 移动热点）不下发 DNS，导致域名无法解析，这里兜底公共 DNS
                    if (dns_missing) {
                        esp_netif_dns_info_t fallback = {};
                        fallback.ip.type = ESP_IPADDR_TYPE_V4;
                        if (esp_netif_str_to_ip4("223.5.5.5", &fallback.ip.u_addr.ip4) == ESP_OK &&
                            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &fallback) == ESP_OK) {
                            ESP_LOGW(TAG, "DHCP 未下发 DNS，已兜底使用 223.5.5.5");
                        }
                        esp_netif_dns_info_t backup = {};
                        backup.ip.type = ESP_IPADDR_TYPE_V4;
                        if (esp_netif_str_to_ip4("119.29.29.29", &backup.ip.u_addr.ip4) == ESP_OK &&
                            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup) == ESP_OK) {
                            ESP_LOGI(TAG, "备用 DNS 已设置为 119.29.29.29");
                        }
                    }

                    // 启动截图服务，方便主机侧查看设备真实画面（GET /shot.bmp）
                    esp_netif_ip_info_t ip_info = {};
                    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        ESP_LOGI(TAG, "设备 IP: " IPSTR "，截图地址: http://" IPSTR ":8080/shot.bmp",
                                 IP2STR(&ip_info.ip), IP2STR(&ip_info.ip));
                    }
                    if (display_ != nullptr) {
                        // 挂载 SD 卡（非致命：没插卡/格式不对只告警，不影响主功能）
                        SdcardService::GetInstance().Init();
                        ScreenshotServerStart(display_);
                    }
                }
            }
            if (callback) {
                callback(event, data);
            }
        });
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        // 本板未把充电芯片 STAT 脚接到 SoC，改用电压趋势 + 满电平台判定（见 GetBatteryStatus）
        BatteryStatus st = GetBatteryStatus();
        level = st.percent;
        charging = st.charging;
        discharging = st.discharging;
        // 诊断日志：状态变化或电量每跨 5% 打印一次（不算高频）
        static int last_logged_pct = -1;
        static int last_logged_chg = -1;
        int pct_delta = (last_logged_pct < 0) ? 100 : (st.percent > last_logged_pct ? st.percent - last_logged_pct : last_logged_pct - st.percent);
        if ((int)st.charging != last_logged_chg || pct_delta >= 5) {
            ESP_LOGI(TAG, "电池: %d%% %umV known=%d charging=%d full=%d discharging=%d",
                     st.percent, (unsigned)st.voltage_mv, (int)st.known, (int)st.charging, (int)st.full, (int)st.discharging);
            last_logged_pct = st.percent;
            last_logged_chg = (int)st.charging;
        }
        return true;
    }
};

DECLARE_BOARD(CustomBoard);
