#pragma once
#include <string>
#include <mutex>
#include "esp_http_client.h"

// 单日预报（和风天气 3 天预报的每一天）
struct WeatherDaily {
    std::string date;       // 2026-11-17
    std::string week;       // 周二
    std::string text_day;   // 白天天气：晴
    std::string text_night; // 夜间天气：阴
    std::string temp_min;   // 最低温
    std::string temp_max;   // 最高温
    std::string wind_dir;   // 东北风
    std::string wind_scale; // 1-3
    std::string sunrise;    // 06:29
    std::string sunset;     // 17:00
};

// 天气数据结构
struct WeatherData {
    std::string city;     // 城市名
    std::string temp;     // 温度（字符串）
    std::string text;     // 天气描述（如"晴"、"多云"）
    std::string update_time;
    bool valid = false;

    // ===== 扩展维度（和风天气提供；免密钥源能填多少填多少，未取到为空）=====
    std::string feels_like;   // 体感温度
    std::string humidity;     // 相对湿度 %
    std::string wind_dir;     // 风向：东北风
    std::string wind_scale;   // 风力等级：3
    std::string vis;          // 能见度 km
    std::string pressure;     // 气压 hPa
    std::string aqi;          // 空气质量指数
    std::string aqi_category; // 优 / 良 / 轻度污染 ...
    std::string pm25;         // PM2.5
    std::string sunrise;      // 今日日出 HH:MM
    std::string sunset;       // 今日日落 HH:MM
    std::string warning;      // 当前生效预警，如"大雾黄色"；无预警为空
    WeatherDaily daily[3];    // 未来 3 天（含今天）
    int daily_count = 0;
};

// 天气管理器
// 优先走和风天气（QWeather）：实时 + 空气质量 + 3 天预报 + 预警；
// 凭证未配置或请求失败时自动回退免密钥源（中国天气网 → Open-Meteo）。
class WeatherManager {
public:
    static WeatherManager& getInstance();

    // 更新天气数据（网络请求，耗时较长，应在后台任务中调用）
    // 返回 true 表示更新成功，false 表示失败（网络错误等）
    bool update();

    // 获取最新天气数据（线程安全，直接读取缓存）
    // 即使网络断开，也能返回上次成功获取的数据
    WeatherData getLatestData() {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_data_;
    }

    // 调试用：返回天气摘要字符串（调用方无需在栈上放完整 WeatherData，
    // 避免 -Wframe-larger-than=1536 在 HTTP handler 里报错）
    std::string getWeatherSummary();

    // 设置 API 密钥和主机（从配置中读取）
    void setApiConfig(const char* key, const char* host);

    // 是否已配置和风天气凭证
    bool hasQWeather() const { return !api_key_.empty() && !api_host_.empty(); }

    // 从 NVS 命名空间 "weather" 读取 api_host / api_key / city / loc_id / loc_name
    // 供运行时（不重新编译）修改天气源配置
    void loadConfigFromNvs();

    // 当前查询城市（IP 定位 > NVS 手动配置 > config.h WEATHER_CITY）
    const std::string& city() const { return city_; }

    // 手动指定城市（MCP self.weather.set_city 用）：清定位缓存并跳过后续 IP 定位
    // adm：可选上级行政区划（省/市），用于同名城市消歧（参考 DuDuClock 的做法）
    void SetCity(const std::string& city, const std::string& adm = "");

    // 当前城市来源（手动配置 / 上次定位缓存 / IP 定位 / 默认）——诊断用
    const std::string& citySource() const { return city_source_; }
    // 当前解析到的和风 LocationID——诊断用
    const std::string& locationId() const { return loc_id_; }

    // 通过外部工具（如 MCP）直接写入天气数据
    // 适用于不走板载 HTTP 天气接口，而由 AI 侧先查好天气再下发到设备
    bool updateFromExternal(const std::string& city,
                            const std::string& weather_text,
                            const std::string& temperature,
                            const std::string& update_time = "");

private:
    WeatherManager();
    mutable std::mutex mutex_;   // 保护 latest_data_：多任务并发读写 std::string 会 UB
    WeatherData latest_data_;

    // API 配置
    std::string api_key_;
    std::string api_host_;
    std::string city_;
    std::string loc_id_;    // 和风天气 LocationID（geo 查询结果，缓存在 NVS）
    std::string loc_name_;  // 和风返回的城市名（用于显示）
    std::string adm_;       // 上级行政区划（手动配置或 geo 返回的 adm1），同名城市消歧用
    std::string city_source_ = "默认";   // 城市来源描述
    std::string ip_cache_city_;          // NVS 缓存的上次 IP 定位城市
    std::string ip_cache_region_;        // NVS 缓存的上次 IP 定位省份
    std::string backend_cache_city_;     // NVS 缓存的后端地址城市
    std::string backend_cache_adm_;      // NVS 缓存的后端地址省份
    std::string backend_cache_loc_id_;   // NVS 缓存的后端 LocationID

    // IP 定位状态（ip-api.com）
    bool manual_city_ = false;   // 城市被手动指定，跳过 IP 定位
    bool ip_located_ = false;    // 本次开机是否已尝试过 IP 定位
    bool has_ip_coord_ = false;  // 是否拿到经纬度（城市名查询失败时兜底）
    float ip_lat_ = 0.0f;
    float ip_lon_ = 0.0f;

    static esp_err_t http_event_handler(esp_http_client_event_t *evt);

    // 通用 HTTP(S) GET：把响应体原始字节写入 out（verify_tls=false 时不校验证书）
    bool httpGetRaw(const std::string& url, int timeout_ms, bool verify_tls, std::string& out);
    // 通用 HTTPS GET：成功时把响应体（自动处理 gzip）写入 out
    bool httpGetJson(const std::string& url, std::string& out, int timeout_ms);

    // 和风天气：解析 LocationID（优先用 NVS 缓存，其次 geo 查询，最后用城市代码兜底）
    std::string ResolveLocationId();
    bool updateQWeather();

    // 后端地址：从 xiaozhi-server 的 /xiaozhi/weather 拿实际所在地 + LocationID
    bool LocateFromBackend();

    // IP 定位：拿到实际所在城市 + 经纬度（手动配置城市时直接跳过）
    bool LocateByIp();

    // 无和风 Key 时的免密钥天气源：主用中国天气网(数据接口)，备用 Open-Meteo
    bool updateItboy();
    bool updateOpenMeteo();
    // 用 Open-Meteo 覆盖 itboy 的清晨脏实时值（温度/体感/湿度）。itboy 的 wendu 常停留在
    // 清晨旧值（如 0.0°C / -6.7°C），与实况偏差大；三天预报保留 itboy——用户手机天气
    // （小米/华为，中国天气网系）与 itboy 同源，口径一致。天气文字/风力/AQI 等均保留 itboy。
    bool overlayRealtimeFromOpenMeteo();
};
