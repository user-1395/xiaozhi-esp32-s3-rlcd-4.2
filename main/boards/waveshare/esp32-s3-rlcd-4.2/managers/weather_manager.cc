#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include "../config.h"   // WEATHER_LAT / WEATHER_LON / WEATHER_CITY
#include "../date_utils.h"
#include <settings.h>
#if __has_include("weather_qweather_secret.h")
#include "weather_qweather_secret.h"
#endif
#include <string.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>

static const char *TAG = "WeatherManager";

// HTTP 响应上下文：按需增长，不再受固定 8KB 缓冲限制（避免大响应被静默截断）
struct HttpCtx {
    std::string data;
};

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->user_data != nullptr && evt->data != nullptr && evt->data_len > 0) {
                static_cast<HttpCtx*>(evt->user_data)->data.append(
                    static_cast<const char*>(evt->data), evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
    // 编译期默认值：config.h（空）或 gitignore 的 weather_qweather_secret.h
    city_ = WEATHER_CITY;
#ifdef QWEATHER_HOST
    api_host_ = QWEATHER_HOST;
#endif
#ifdef QWEATHER_KEY
    api_key_ = QWEATHER_KEY;
#endif
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::setApiConfig(const char* key, const char* host) {
    api_key_ = key;
    api_host_ = host;
}

bool WeatherManager::updateFromExternal(const std::string& city,
                                        const std::string& weather_text,
                                        const std::string& temperature,
                                        const std::string& update_time) {
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    if (city.empty() || weather_text.empty() || temperature.empty()) {
        ESP_LOGW(TAG, "外部天气数据无效：city/text/temp 不能为空");
        return false;
    }

    d.city = city;
    d.text = weather_text;
    d.temp = temperature;
    d.update_time = update_time.empty() ? "mcp" : update_time;
    d.valid = true;

    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C",
             d.city.c_str(),
             d.text.c_str(),
             d.temp.c_str());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
    }
    return true;
}

// GZIP 解压到 std::string：按块增长，响应体多大都不会被固定缓冲截断
static bool gunzip_to_string(const std::string& in, std::string& out) {
    out.clear();   // 必须清空：out 是复用的响应缓冲，追加会拼上前一个接口的 JSON
    if (in.size() < 18 || (unsigned char)in[0] != 0x1f || (unsigned char)in[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)in.data();
    strm.avail_in = (uInt)in.size();
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    static char buf[2048];   // 静态缓冲：避免 2KB 栈帧触发 -Werror=frame-larger-than
    int ret = Z_OK;
    do {
        strm.next_out = (Bytef*)buf;
        strm.avail_out = sizeof(buf);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&strm); return false; }
        out.append(buf, sizeof(buf) - strm.avail_out);
    } while (ret == Z_OK);   // 必须一直解到 Z_STREAM_END：压缩流可能先耗尽 avail_in 但仍有输出待取
    inflateEnd(&strm);
    return ret == Z_STREAM_END;
}

// 判断城市名是否可用于 UI 展示（排除 "Ip" 这类占位值）
static bool is_valid_display_city(const char* city) {
    if (city == nullptr || city[0] == '\0') {
        return false;
    }
    std::string normalized(city);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized != "ip" && normalized != "auto_ip" && normalized != "unknown";
}

// 优先使用更像“真实地名”的字段，避免把 "Ip" 显示到页面上
static std::string pick_city_name_for_display(cJSON* first_city, const std::string& fallback_city) {
    const char* keys[] = {"adm2", "adm1", "name"};
    for (const char* key : keys) {
        cJSON* item = cJSON_GetObjectItem(first_city, key);
        if (item && cJSON_IsString(item) && is_valid_display_city(item->valuestring)) {
            return item->valuestring;
        }
    }
    return fallback_city;
}

// 通用 HTTP(S) GET：把响应体原始字节读入 out（dynamic，无固定上限）
bool WeatherManager::httpGetRaw(const std::string& url, int timeout_ms, bool verify_tls, std::string& out) {
    HttpCtx ctx;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = http_event_handler;
    config.user_data = &ctx;
    config.timeout_ms = timeout_ms;
    if (verify_tls) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-RLCD-Weather");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.data.empty()) {
        ESP_LOGW(TAG, "HTTP 请求失败 (err=%d, status=%d, len=%d) url=%.110s",
                 err, status, (int)ctx.data.size(), url.c_str());
        return false;
    }
    out.swap(ctx.data);
    return true;
}

// 通用 HTTPS GET：成功时把响应体（自动处理 gzip）写入 out
bool WeatherManager::httpGetJson(const std::string& url, std::string& out, int timeout_ms) {
    std::string raw;
    if (!httpGetRaw(url, timeout_ms, true, raw)) {
        return false;
    }
    if (gunzip_to_string(raw, out)) {
        return true;
    }
    out.swap(raw);
    return true;
}

// URL 编码（城市名是中文，必须 percent-encode）
static std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// "2026-11-17" -> "周二"
static std::string DateToWeek(const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) {
        return "";
    }
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return std::string("周") + date_utils::WeekdayCharZh(w);
}

// 只保留数字："高温 17℃" -> "17"
static std::string DigitsOnly(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c >= '0' && c <= '9') {
            out.push_back(c);
        }
    }
    return out;
}

// 从 NVS 命名空间 "weather" 读运行时可改配置
void WeatherManager::loadConfigFromNvs() {
    Settings settings("weather", false);
    std::string host = settings.GetString("api_host");
    std::string key = settings.GetString("api_key");
    std::string city = settings.GetString("city");
    std::string adm = settings.GetString("adm");
    if (!host.empty() && !key.empty()) {
        api_host_ = host;
        api_key_ = key;
    }
    if (!city.empty()) {
        city_ = city;
        adm_ = adm;
        manual_city_ = true;   // NVS 里手动指定了城市：最高优先级，跳过 IP 定位
    }
    std::string loc_id = settings.GetString("loc_id");
    std::string loc_name = settings.GetString("loc_name");
    if (!loc_id.empty()) {
        loc_id_ = loc_id;
        loc_name_ = loc_name;
    }
    // 和风 geo 缓存的经纬度：新版空气质量接口按经纬度查询，避免每次开机重新 geo
    std::string loc_coord = settings.GetString("loc_coord");
    float cla = 0.0f, clo = 0.0f;
    if (sscanf(loc_coord.c_str(), "%f,%f", &cla, &clo) == 2) {
        loc_lat_ = cla;
        loc_lon_ = clo;
    }
    // 后端地址缓存：后端是权威来源，开机先用上次结果，网络就绪后 LocateFromBackend() 刷新。
    backend_cache_city_ = settings.GetString("backend_city");
    backend_cache_adm_ = settings.GetString("backend_adm");
    backend_cache_loc_id_ = settings.GetString("backend_loc_id");
    if (!manual_city_ && !backend_cache_city_.empty()) {
        city_ = backend_cache_city_;
        adm_ = backend_cache_adm_;
        loc_id_.clear();
        loc_name_.clear();
        if (!backend_cache_loc_id_.empty()) {
            loc_id_ = backend_cache_loc_id_;
            loc_name_ = backend_cache_city_;
        }
    }
    // 上次 IP 定位结果只作「开机先用」的缓存值；有后端缓存时不再用 IP 缓存覆盖。
    ip_cache_city_ = settings.GetString("ip_city");
    ip_cache_region_ = settings.GetString("ip_region");
    if (!manual_city_ && backend_cache_city_.empty() && !ip_cache_city_.empty()) {
        city_ = ip_cache_city_;
        std::string coord = settings.GetString("ip_coord");
        float la = 0.0f, lo = 0.0f;
        if (sscanf(coord.c_str(), "%f,%f", &la, &lo) == 2) {
            ip_lat_ = la;
            ip_lon_ = lo;
            has_ip_coord_ = true;
        }
    }
    city_source_ = manual_city_ ? "手动配置"
                   : (!backend_cache_city_.empty() ? "后端缓存"
                      : (ip_cache_city_.empty() ? "默认" : "上次定位缓存"));
    // LocationID 必须与当前城市同源，否则会拿旧城市的 ID 查新城市（历史上南京/常州串过）。
    if (!loc_id_.empty() && !loc_name_.empty() && loc_name_ != city_) {
        loc_id_.clear();
        loc_name_.clear();
    }
    ESP_LOGI(TAG, "天气配置: 城市=%s(来源=%s, 上级=%s), 和风=%s, LocationID=%s",
             city_.c_str(), city_source_.c_str(), adm_.empty() ? "-" : adm_.c_str(),
             hasQWeather() ? "已配置" : "未配置",
             loc_id_.empty() ? "(待查询)" : loc_id_.c_str());
}

// 解析三类免费 IP 定位接口的响应（字段名各不相同），成功返回 true 并填充经纬度/城市/省份。
// 参考 xinnan-tech/xiaozhi-esp32-server 的 get_ip_info()：IP 只作初值，最终仍要用天气商
// geo 接口归一化成正确的地名（IP 库只到城市级，热点/CGNAT 下必然偏）。
static bool parse_ip_geo(const std::string& body, const char* source,
                         std::string& out_city, std::string& out_region,
                         float& out_lat, float& out_lon) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "IP 定位(%s)响应非 JSON", source);
        return false;
    }
    auto str_of = [&](const char* k) -> std::string {
        cJSON* it = cJSON_GetObjectItem(root, k);
        return (it && cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring) : std::string();
    };
    auto num_of = [&](const char* k, float& dst) -> bool {
        cJSON* it = cJSON_GetObjectItem(root, k);
        if (it && cJSON_IsNumber(it)) { dst = (float)it->valuedouble; return true; }
        if (it && cJSON_IsString(it) && it->valuestring[0]) { dst = (float)atof(it->valuestring); return true; }
        return false;
    };

    std::string status = str_of("status");
    std::string err = str_of("error");
    bool looks_ok = err.empty() && (status.empty() || status == "success");
    bool ok = false;
    if (looks_ok) {
        out_city = str_of("city");
        out_region = str_of("regionName");
        if (out_region.empty()) out_region = str_of("region");
        float la = 0.0f, lo = 0.0f;
        bool has_lat = num_of("lat", la) || num_of("latitude", la);
        bool has_lon = num_of("lon", lo) || num_of("longitude", lo);
        if (!(has_lat && has_lon)) {
            std::string loc = str_of("loc");   // ipinfo.io: "32.0607,118.7630"
            if (!loc.empty() && sscanf(loc.c_str(), "%f,%f", &la, &lo) == 2) {
                has_lat = has_lon = true;
            }
        }
        if (has_lat && has_lon) { out_lat = la; out_lon = lo; }
        ok = !out_city.empty();
        if (ok) {
            ESP_LOGI(TAG, "IP 定位(%s)成功: %s %s (%.4f, %.4f)", source,
                     out_region.c_str(), out_city.c_str(), (double)out_lat, (double)out_lon);
        }
    } else {
        ESP_LOGW(TAG, "IP 定位(%s)返回失败: status=%s error=%s", source,
                 status.c_str(), err.c_str());
    }
    cJSON_Delete(root);
    return ok;
}

// 后端地址：GET /xiaozhi/weather，后端返回 {code:0,data:{city,adm,location_id}}。
// 设备不再自己用出口 IP 判城市；后端位置由 config.yaml 的 device_location 配置。
bool WeatherManager::LocateFromBackend() {
    if (manual_city_) {
        return true;
    }
    std::string body;
    if (!httpGetJson(WEATHER_BACKEND_GEO_URL, body, 10000)) {
        ESP_LOGW(TAG, "后端天气地址请求失败");
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "后端天气地址响应非 JSON");
        return false;
    }
    cJSON* code = cJSON_GetObjectItem(root, "code");
    cJSON* data = cJSON_GetObjectItem(root, "data");
    bool code_ok = code && ((cJSON_IsNumber(code) && code->valueint == 0) ||
                            (cJSON_IsString(code) && strcmp(code->valuestring, "0") == 0));
    if (!code_ok || data == nullptr || !cJSON_IsObject(data)) {
        cJSON* msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGW(TAG, "后端天气地址返回失败: %s",
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return false;
    }
    auto str_of = [&](const char* k) -> std::string {
        cJSON* it = cJSON_GetObjectItem(data, k);
        return (it && cJSON_IsString(it) && it->valuestring) ? std::string(it->valuestring) : std::string();
    };
    std::string city = str_of("city");
    std::string adm = str_of("adm");
    std::string id = str_of("location_id");
    if (city.empty() || id.empty()) {
        ESP_LOGW(TAG, "后端天气地址缺少城市或 LocationID");
        cJSON_Delete(root);
        return false;
    }
    city_ = city;
    adm_ = adm;
    loc_id_ = id;
    loc_name_ = city;
    has_ip_coord_ = false;   // 后端已给出 LocationID，禁止再用出口 IP 经纬度反查
    city_source_ = "后端服务";
    Settings wr("weather", true);
    wr.SetString("backend_city", city_);
    wr.SetString("backend_adm", adm_);
    wr.SetString("backend_loc_id", loc_id_);
    ESP_LOGI(TAG, "后端天气地址: %s %s (%s)", adm_.c_str(), city_.c_str(), loc_id_.c_str());
    cJSON_Delete(root);
    return true;
}

// IP 定位：依次尝试 ip-api → ipapi.co → ipinfo.io（都是免密钥公开接口）。
// 仅在没有手动配置城市时使用；全部失败则回退上次定位缓存，再不行沿用当前城市。
bool WeatherManager::LocateByIp() {
    if (manual_city_) {
        ESP_LOGI(TAG, "城市为手动配置(%s)，跳过 IP 定位", city_.c_str());
        return true;
    }
    const char* urls[3] = {WEATHER_IP_GEO_URL, WEATHER_IP_GEO_URL_ALT1, WEATHER_IP_GEO_URL_ALT2};
    const char* names[3] = {"ip-api", "ipapi.co", "ipinfo.io"};
    std::string city, region;
    float lat = 0.0f, lon = 0.0f;
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        std::string body;
        if (!httpGetJson(urls[i], body, 8000)) {
            ESP_LOGW(TAG, "IP 定位(%s)请求失败", names[i]);
            continue;
        }
        city.clear();
        region.clear();
        ok = parse_ip_geo(body, names[i], city, region, lat, lon);
    }
    if (!ok) {
        if (!ip_cache_city_.empty()) {
            city_ = ip_cache_city_;
            city_source_ = "上次定位缓存";
            ESP_LOGW(TAG, "IP 定位全部失败，沿用上次定位缓存城市 %s", city_.c_str());
            return true;
        }
        ESP_LOGW(TAG, "IP 定位全部失败，沿用城市 %s", city_.c_str());
        return false;
    }

    city_ = city;
    if (!region.empty()) {
        adm_ = region;   // 仅作 geo 消歧提示（英文省份会被自动忽略）
    }
    if (lat != 0.0f || lon != 0.0f) {
        ip_lat_ = lat;
        ip_lon_ = lon;
        has_ip_coord_ = true;
    }
    // 城市变了，之前缓存的 LocationID 必须作废
    loc_id_.clear();
    loc_name_.clear();
    city_source_ = "IP 定位";
    Settings wr("weather", true);
    wr.SetString("ip_city", city_);
    wr.SetString("ip_region", adm_);
    char coord[40];
    snprintf(coord, sizeof(coord), "%.4f,%.4f", (double)ip_lon_, (double)ip_lat_);
    wr.SetString("ip_coord", coord);
    return true;
}

// 手动指定城市：清掉 LocationID 缓存、禁用经纬度反查，下次 update() 按新城市重新解析
void WeatherManager::SetCity(const std::string& city, const std::string& adm) {
    if (city.empty()) {
        return;
    }
    city_ = city;
    adm_ = adm;
    loc_id_.clear();
    loc_name_.clear();
    has_ip_coord_ = false;   // 手动城市优先，禁止用 IP 经纬度反查，避免被出口 IP 带偏
    manual_city_ = true;
    city_source_ = "手动配置";
    Settings wr("weather", true);
    wr.SetString("city", city_);
    wr.SetString("adm", adm_);
    wr.SetString("loc_id", "");
    wr.SetString("loc_name", "");
    ESP_LOGI(TAG, "天气城市已手动设置为 %s(上级=%s)", city_.c_str(), adm_.empty() ? "-" : adm_.c_str());
}

// 和风 LocationID：NVS 缓存 > 经纬度反查 > 城市名(+上级行政区划) > 城市代码兜底
std::string WeatherManager::ResolveLocationId() {
    if (!loc_id_.empty()) {
        return loc_id_;
    }
    auto lookup = [&](const std::string& location_param, const std::string& adm_param) -> bool {
        std::string body;
        // std::string 拼接避免固定缓冲截断（中文 percent-encode 后长度不可控）
        std::string url = "https://" + api_host_ + "/geo/v2/city/lookup?location=" + location_param;
        if (!adm_param.empty()) {
            url += "&adm=" + adm_param;
        }
        url += "&lang=zh&key=" + api_key_;
        if (!httpGetJson(url, body, 12000)) {
            return false;
        }
        bool ok = false;
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* code = cJSON_GetObjectItem(root, "code");
            cJSON* locs = cJSON_GetObjectItem(root, "location");
            if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 &&
                locs && cJSON_GetArraySize(locs) > 0) {
                cJSON* first = cJSON_GetArrayItem(locs, 0);
                cJSON* id = cJSON_GetObjectItem(first, "id");
                cJSON* name = cJSON_GetObjectItem(first, "name");
                if (id && cJSON_IsString(id)) {
                    std::string raw_name = (name && cJSON_IsString(name)) ? std::string(name->valuestring) : std::string();
                    // 优先取 adm2（地级市），避免把「玄武区」这类区县名显示到屏幕上
                    std::string display = pick_city_name_for_display(first, raw_name);
                    if (display.size() > 3 && display.compare(display.size() - 3, 3, "市") == 0) {
                        display.erase(display.size() - 3);   // 「南京市」→「南京」
                    }
                    loc_id_ = id->valuestring;
                    loc_name_ = display;
                    // 保存 geo 返回的经纬度：新版空气质量接口 /airquality/v1/current/{lat}/{lon} 需要
                    cJSON* jlat = cJSON_GetObjectItem(first, "lat");
                    cJSON* jlon = cJSON_GetObjectItem(first, "lon");
                    if (jlat && cJSON_IsString(jlat)) loc_lat_ = (float)atof(jlat->valuestring);
                    if (jlon && cJSON_IsString(jlon)) loc_lon_ = (float)atof(jlon->valuestring);
                    cJSON* adm1 = cJSON_GetObjectItem(first, "adm1");
                    if (adm1 && cJSON_IsString(adm1) && adm1->valuestring[0] != 0) {
                        adm_ = adm1->valuestring;
                    }
                    // IP 定位源（如 ipinfo）可能返回英文城市名，用和风解析出的中文地名回填，保证显示/播报一致
                    if (city_source_ == "IP 定位" && !display.empty()) {
                        city_ = display;
                    }
                    Settings wr("weather", true);
                    wr.SetString("loc_id", loc_id_);
                    wr.SetString("loc_name", loc_name_);
                    char coordbuf[40];
                    snprintf(coordbuf, sizeof(coordbuf), "%.4f,%.4f", (double)loc_lat_, (double)loc_lon_);
                    wr.SetString("loc_coord", coordbuf);
                    ESP_LOGI(TAG, "和风 geo 定位成功: %s (%s, 上级=%s)",
                             loc_name_.c_str(), loc_id_.c_str(), adm_.c_str());
                    ok = true;
                }
            }
            cJSON_Delete(root);
        }
        return ok;
    };

    // 1) 经纬度优先：和风按坐标反查最接近真实位置，比 IP 库给的城市名更可靠
    if (has_ip_coord_) {
        char coord[40];
        snprintf(coord, sizeof(coord), "%.4f,%.4f", (double)ip_lon_, (double)ip_lat_);
        if (lookup(coord, "")) {
            return loc_id_;
        }
    }
    // 2) 城市名；有中文上级行政区划时带上 adm 消歧（同名城市，如「朝阳」）
    if (!city_.empty()) {
        bool adm_is_chinese = false;
        for (size_t i = 0; i < adm_.size(); i++) {
            if ((unsigned char)adm_[i] >= 0x80) { adm_is_chinese = true; break; }
        }
        if (adm_is_chinese && lookup(url_encode(city_), url_encode(adm_))) {
            return loc_id_;
        }
        if (lookup(url_encode(city_), "")) {
            return loc_id_;
        }
    }
    loc_id_ = WEATHER_CITY_CODE;
    ESP_LOGW(TAG, "geo 定位失败，直接用城市代码 %s", loc_id_.c_str());
    return loc_id_;
}

// 和风天气：实时 + 空气质量 + 3 天预报 + 预警
bool WeatherManager::updateQWeather() {
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    const std::string loc = ResolveLocationId();
    if (loc.empty()) {
        return false;
    }
    std::string body;
    std::string url;
    auto str_of = [](cJSON* obj, const char* k) -> std::string {
        cJSON* it = obj ? cJSON_GetObjectItem(obj, k) : nullptr;
        if (it == nullptr) {
            return "";
        }
        if (cJSON_IsString(it)) {
            return it->valuestring;
        }
        if (cJSON_IsNumber(it)) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d", it->valueint);
            return buf;
        }
        return "";
    };

    // 1) 实时天气
    url = "https://" + api_host_ + "/v7/weather/now?location=" + loc + "&lang=zh&key=" + api_key_;
    if (!httpGetJson(url, body, 15000)) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "和风实时天气解析失败 (head=%.80s)", body.c_str());
        return false;
    }
    cJSON* code = cJSON_GetObjectItem(root, "code");
    cJSON* now = cJSON_GetObjectItem(root, "now");
    if (!(code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0 && now)) {
        ESP_LOGW(TAG, "和风实时天气返回异常 code=%s",
                 (code && cJSON_IsString(code)) ? code->valuestring : "null");
        cJSON_Delete(root);
        return false;
    }
    d.temp = str_of(now, "temp");
    d.text = str_of(now, "text");
    d.feels_like = str_of(now, "feelsLike");
    d.humidity = str_of(now, "humidity");
    d.wind_dir = str_of(now, "windDir");
    d.wind_scale = str_of(now, "windScale");
    d.vis = str_of(now, "vis");
    d.pressure = str_of(now, "pressure");
    std::string upd = str_of(root, "updateTime");
    d.update_time = upd.empty() ? "qweather" : upd;
    d.city = loc_name_.empty() ? city_ : loc_name_;
    cJSON_Delete(root);

    // 2) 空气质量（新版接口：旧 /v7/air/now 已于 2026-06 停服）
    //    GET /airquality/v1/current/{lat}/{lon}，从 indexes 里取中国标准 cn-mee，
    //    从 pollutants 里取 pm2p5。先清空旧值，失败不保留上一轮数据。
    d.aqi.clear();
    d.aqi_category.clear();
    d.pm25.clear();
    if (loc_lat_ != 0.0f && loc_lon_ != 0.0f) {
        char aqicoord[48];
        snprintf(aqicoord, sizeof(aqicoord), "%.4f/%.4f", (double)loc_lat_, (double)loc_lon_);
        url = "https://" + api_host_ + "/airquality/v1/current/" + aqicoord + "?lang=zh&key=" + api_key_;
        if (httpGetJson(url, body, 12000)) {
            cJSON* aroot = cJSON_Parse(body.c_str());
            if (aroot) {
                cJSON* indexes = cJSON_GetObjectItem(aroot, "indexes");
                if (indexes && cJSON_IsArray(indexes)) {
                    int ic = cJSON_GetArraySize(indexes);
                    for (int i = 0; i < ic; ++i) {
                        cJSON* ix = cJSON_GetArrayItem(indexes, i);
                        std::string icode = str_of(ix, "code");
                        if (icode == "cn-mee") {   // 中国国标 AQI，与手机天气口径一致
                            d.aqi = str_of(ix, "aqiDisplay");
                            if (d.aqi.empty()) d.aqi = str_of(ix, "aqi");
                            d.aqi_category = str_of(ix, "category");
                            break;
                        }
                    }
                }
                cJSON* pollutants = cJSON_GetObjectItem(aroot, "pollutants");
                if (pollutants && cJSON_IsArray(pollutants)) {
                    int pc = cJSON_GetArraySize(pollutants);
                    for (int i = 0; i < pc; ++i) {
                        cJSON* p = cJSON_GetArrayItem(pollutants, i);
                        if (str_of(p, "code") == "pm2p5") {
                            cJSON* conc = cJSON_GetObjectItem(p, "concentration");
                            d.pm25 = str_of(conc, "value");
                            break;
                        }
                    }
                }
                cJSON_Delete(aroot);
            } else {
                ESP_LOGW(TAG, "和风空气质量解析失败 head=%.80s", body.c_str());
            }
        }
    }

    // 3) 3 天预报（含日出日落 / 最高最低 / 风）
    //    先重置预报计数与日出日落：本子请求失败时不保留上一轮数据
    d.daily_count = 0;
    d.sunrise.clear();
    d.sunset.clear();
    url = "https://" + api_host_ + "/v7/weather/3d?location=" + loc + "&lang=zh&key=" + api_key_;
    if (httpGetJson(url, body, 15000)) {
        cJSON* droot = cJSON_Parse(body.c_str());
        if (droot) {
            cJSON* daily = cJSON_GetObjectItem(droot, "daily");
            int n = daily ? cJSON_GetArraySize(daily) : 0;
            d.daily_count = (n > 3) ? 3 : n;
            for (int i = 0; i < d.daily_count; ++i) {
                cJSON* day = cJSON_GetArrayItem(daily, i);
                WeatherDaily& wd = d.daily[i];
                wd.date = str_of(day, "fxDate");
                wd.week = DateToWeek(wd.date);
                wd.text_day = str_of(day, "textDay");
                wd.text_night = str_of(day, "textNight");
                wd.temp_min = str_of(day, "tempMin");
                wd.temp_max = str_of(day, "tempMax");
                wd.wind_dir = str_of(day, "windDirDay");
                wd.wind_scale = str_of(day, "windScaleDay");
                wd.sunrise = str_of(day, "sunrise");
                wd.sunset = str_of(day, "sunset");
            }
            if (d.daily_count > 0) {
                d.sunrise = d.daily[0].sunrise;
                d.sunset = d.daily[0].sunset;
            } else {
                cJSON* dcode = cJSON_GetObjectItem(droot, "code");
                ESP_LOGW(TAG, "和风 3 天预报无 daily code=%s head=%.60s",
                         (dcode && cJSON_IsString(dcode)) ? dcode->valuestring : "null", body.c_str());
            }
            cJSON_Delete(droot);
        } else {
            ESP_LOGW(TAG, "和风 3 天预报解析失败 head=%.80s", body.c_str());
        }
    }

    // 4) 预警：旧 /v7/warning/now 已于 2026-10 停服（返回 403 Deprecated）。
    //    预警非关键维度，留空即可，避免每次请求白等超时。新版预警接口待固件后续升级再接。
    d.warning.clear();

    d.valid = true;
    ESP_LOGI(TAG, "天气更新成功(和风): %s %s %s°C 体感%s 湿度%s%% %s%s级 AQI %s %s 日出%s 日落%s%s%s",
             d.city.c_str(), d.text.c_str(), d.temp.c_str(),
             d.feels_like.c_str(), d.humidity.c_str(),
             d.wind_dir.c_str(), d.wind_scale.c_str(),
             d.aqi.c_str(), d.aqi_category.c_str(),
             d.sunrise.c_str(), d.sunset.c_str(),
             d.warning.empty() ? "" : " 预警:",
             d.warning.c_str());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
    }
    return true;
}

bool WeatherManager::update() {
    // 1) 优先和风天气（维度最全：实时 / 空气质量 / 3 天预报 / 预警）
    if (hasQWeather()) {
        // 每次开机只解析一次地址：后端优先，失败回退后端缓存 / 配置默认城市 / IP 定位
        if (!ip_located_) {
            ip_located_ = true;
            if (!manual_city_ && !LocateFromBackend()) {
                if (!backend_cache_city_.empty()) {
                    city_ = backend_cache_city_;
                    adm_ = backend_cache_adm_;
                    loc_id_.clear();
                    loc_name_.clear();
                    if (!backend_cache_loc_id_.empty()) {
                        loc_id_ = backend_cache_loc_id_;
                        loc_name_ = backend_cache_city_;
                    }
                    has_ip_coord_ = false;
                    city_source_ = "后端缓存";
                    ESP_LOGW(TAG, "后端地址不可用，沿用后端缓存城市 %s", city_.c_str());
                } else if (WEATHER_CITY[0] != '\0') {
                    // 后端不可用时用编译期配置的城市兜底，绝不回退到出口 IP（IP 常被判到邻市）
                    city_ = WEATHER_CITY;
                    loc_id_.clear();      // 必须清掉旧城市的 LocationID，否则仍按南京查天气
                    loc_name_.clear();
                    has_ip_coord_ = false;
                    city_source_ = "配置默认";
                    ESP_LOGW(TAG, "后端地址不可用，使用配置默认城市 %s", city_.c_str());
                } else {
                    LocateByIp();
                }
            }
        }
        if (updateQWeather()) {
            return true;
        }
        ESP_LOGW(TAG, "和风天气更新失败，回退免密钥数据源");
    }
    // 2) 免密钥兜底：中国天气网 -> Open-Meteo
    if (updateItboy()) {
        // itboy 的 wendu 字段常停留在清晨旧值（曾观测到 0.0°C / -6.7°C 而同刻实况更高），
        // 用 Open-Meteo 覆盖温度/体感/湿度三项实时值。三天预报保留 itboy 原值——用户手机
        // （小米/华为天气）用的中国天气网数据与 itboy 同源，口径一致，覆盖反而引入偏差。
        overlayRealtimeFromOpenMeteo();
        return true;
    }
    return updateOpenMeteo();
}

// WMO 天气代码 → 中文描述（Open-Meteo 使用的编码）
static const char* wmo_code_to_zh(int code) {
    switch (code) {
        case 0:  return "晴";
        case 1:  return "晴间多云";
        case 2:  return "多云";
        case 3:  return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: case 66: case 67: return "冻雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: return "阵雨";
        case 81: return "强阵雨";
        case 82: return "暴雨";
        case 85: case 86: return "阵雪";
        case 95: return "雷阵雨";
        case 96: case 99: return "雷阵雨伴冰雹";
        default: return "未知";
    }
}

// 免密钥天气源的城市参数表：当前城市 → itboy 城市代码 + Open-Meteo 经纬度。
// 之前数据源写死常州，导致切换城市后仍抓常州数据；这里按 city_ 动态选择。
struct CityGeoParam {
    const char* name;
    const char* itboy_code;
    float lat;
    float lon;
};

static const CityGeoParam kCityGeoParams[] = {
    {"北京",   "101010100", 39.9042f, 116.4074f},
    {"昌平",   "101010700", 40.0680f, 116.3430f},   // 回龙观所在区；用户手机天气以此为准
    {"常州",   "101191101", 31.8107f, 119.9740f},
    {"上海",   "101020100", 31.2304f, 121.4737f},
    {"南京",   "101190100", 32.0603f, 118.7969f},
    {"苏州",   "101190401", 31.2989f, 120.5853f},
    {"无锡",   "101190201", 31.4912f, 120.3119f},
    {"杭州",   "101210100", 30.2741f, 120.1551f},
    {"宁波",   "101210401", 29.8683f, 121.5440f},
    {"广州",   "101280100", 23.1291f, 113.2644f},
    {"深圳",   "101280601", 22.5431f, 114.0579f},
    {"成都",   "101270100", 30.5728f, 104.0668f},
    {"重庆",   "101040100", 29.5630f, 106.5516f},
    {"武汉",   "101200100", 30.5928f, 114.3055f},
    {"西安",   "101110100", 34.3416f, 108.9398f},
    {"天津",   "101030100", 39.3434f, 117.3616f},
    {"长沙",   "101250100", 28.2282f, 112.9388f},
    {"青岛",   "101120201", 36.0671f, 120.3826f},
    {"济南",   "101120100", 36.6512f, 117.1201f},
    {"郑州",   "101180100", 34.7466f, 113.6254f},
    {"合肥",   "101220100", 31.8206f, 117.2272f},
    {"厦门",   "101230201", 24.4798f, 118.0894f},
    {"福州",   "101230100", 26.0745f, 119.2965f},
    {"昆明",   "101290100", 25.0389f, 102.7183f},
    {"沈阳",   "101070100", 41.8057f, 123.4315f},
    {"大连",   "101070201", 38.9140f, 121.6147f},
    {"哈尔滨", "101050100", 45.8038f, 126.5349f},
    {"长春",   "101060100", 43.8171f, 125.3235f},
    {"石家庄", "101090100", 38.0428f, 114.5149f},
    {"太原",   "101080100", 37.8706f, 112.5489f},
    {"南昌",   "101240100", 28.6829f, 115.8582f},
    {"南宁",   "101300100", 22.8170f, 108.3665f},
    {"海口",   "101310100", 20.0442f, 110.1999f},
    {"兰州",   "101160100", 36.0611f, 103.8343f},
    {"贵阳",   "101260100", 26.6470f, 106.6302f},
};

static const CityGeoParam* city_geo_param(const std::string& city) {
    for (const auto& e : kCityGeoParams) {
        if (city == e.name) return &e;
    }
    std::string norm = city;
    if (norm.size() >= 3 && norm.compare(norm.size() - 3, 3, "市") == 0) {
        norm.erase(norm.size() - 3);
    }
    for (const auto& e : kCityGeoParams) {
        if (norm == e.name) return &e;
    }
    return nullptr;
}

// 免密钥天气源（主）：中国天气网数据接口（纯 HTTP，国内直连快，无需 Key）
// 返回结构：{"cityInfo":{"city":"成都市"},"data":{"wendu":"12.8","forecast":[{"type":"小雨",...}]}}
bool WeatherManager::updateItboy() {
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    // std::string 拼接避免固定缓冲截断；城市代码按当前 city_ 动态解析
    const CityGeoParam* gp = city_geo_param(city_);
    const char* city_code = (gp != nullptr) ? gp->itboy_code : WEATHER_CITY_CODE;
    std::string url = std::string("http://t.weather.itboy.net/api/weather/city/") + city_code;
    ESP_LOGI(TAG, "中国天气网查询城市 %s (code=%s)", city_.c_str(), city_code);

    std::string body;
    if (!httpGetRaw(url, 8000, false, body)) {
        return false;
    }

    // httpGetRaw 统一带 Accept-Encoding: gzip，itboy 可能返回 gzip 压缩体，
    // 必须先解压再解析，否则 cJSON_Parse 失败并回退到字段不全的 Open-Meteo。
    std::string decoded;
    if (!gunzip_to_string(body, decoded)) {
        decoded.swap(body);
    }

    cJSON* root = cJSON_Parse(decoded.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "中国天气网响应解析失败 (head=%.80s)", decoded.c_str());
        return false;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* wendu = data ? cJSON_GetObjectItem(data, "wendu") : nullptr;
    cJSON* forecast = data ? cJSON_GetObjectItem(data, "forecast") : nullptr;
    cJSON* today = (forecast && cJSON_GetArraySize(forecast) > 0) ? cJSON_GetArrayItem(forecast, 0) : nullptr;
    cJSON* type = today ? cJSON_GetObjectItem(today, "type") : nullptr;

    if (wendu == nullptr || !cJSON_IsString(wendu) || type == nullptr || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "中国天气网响应缺少 wendu/type 字段");
        cJSON_Delete(root);
        return false;
    }

    std::string city = WEATHER_CITY;
    cJSON* city_info = cJSON_GetObjectItem(root, "cityInfo");
    cJSON* city_name = city_info ? cJSON_GetObjectItem(city_info, "city") : nullptr;
    if (city_name && cJSON_IsString(city_name) && city_name->valuestring[0] != 0) {
        city = city_name->valuestring;
        // "成都市" -> "成都"，避免卡片文案过长
        if (city.size() > 6) {
            city = WEATHER_CITY;
        } else if (city.size() >= 3 &&
                   city.compare(city.size() - 3, 3, "市") == 0) {
            city.erase(city.size() - 3);
        }
    }

    d.city = city;
    d.temp = wendu->valuestring;
    d.text = type->valuestring;
    d.update_time = "weather.com.cn";

    // 免密钥源也补齐部分维度：AQI / 湿度 / 风力 / 日出日落 / 3 天预报
    auto it_str = [](cJSON* obj, const char* k) -> std::string {
        cJSON* it = obj ? cJSON_GetObjectItem(obj, k) : nullptr;
        if (it == nullptr) {
            return "";
        }
        if (cJSON_IsString(it)) {
            return it->valuestring;
        }
        if (cJSON_IsNumber(it)) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d", it->valueint);
            return buf;
        }
        return "";
    };
    d.humidity = DigitsOnly(it_str(data, "shidu"));  // itboy 返回 "62%"，UI 会再拼一个 "%"，这里只留数字
    d.aqi = it_str(today, "aqi");
    d.aqi_category = it_str(data, "quality");
    d.pm25 = it_str(data, "pm25");
    d.wind_dir = it_str(today, "fx");
    d.wind_scale = DigitsOnly(it_str(today, "fl"));
    d.sunrise = it_str(today, "sunrise");
    d.sunset = it_str(today, "sunset");
    d.warning.clear();
    d.feels_like.clear();
    d.vis.clear();
    d.pressure.clear();
    d.daily_count = 0;
    int dn = forecast ? cJSON_GetArraySize(forecast) : 0;
    for (int i = 0; i < dn && i < 3; ++i) {
        cJSON* day = cJSON_GetArrayItem(forecast, i);
        WeatherDaily& wd = d.daily[i];
        wd.date = it_str(day, "ymd");
        wd.week = it_str(day, "week");
        wd.text_day = it_str(day, "type");
        wd.text_night = it_str(day, "type");
        wd.temp_max = DigitsOnly(it_str(day, "high"));
        wd.temp_min = DigitsOnly(it_str(day, "low"));
        wd.wind_dir = it_str(day, "fx");
        wd.wind_scale = DigitsOnly(it_str(day, "fl"));
        wd.sunrise = it_str(day, "sunrise");
        wd.sunset = it_str(day, "sunset");
        d.daily_count = i + 1;
    }

    d.valid = true;

    ESP_LOGI(TAG, "天气更新成功(中国天气网): %s %s %s°C",
             d.city.c_str(), d.text.c_str(),
             d.temp.c_str());
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
    }
    return true;
}

// 免密钥天气源（备用）：Open-Meteo（无需注册、无 Key）
// 请求：当前温度 + WMO 天气代码；坐标由 config.h 的 WEATHER_LAT/WEATHER_LON 指定
bool WeatherManager::updateOpenMeteo() {
    static WeatherData d;   // 放静态区：WeatherData 含 27 个 std::string，栈帧会超 1536 限制
    d = WeatherData();
    // std::string 拼接避免固定缓冲截断；经纬度按当前 city_ 动态解析
    const CityGeoParam* gp = city_geo_param(city_);
    const float lat = (gp != nullptr) ? gp->lat : WEATHER_LAT;
    const float lon = (gp != nullptr) ? gp->lon : WEATHER_LON;
    char coords[80];
    snprintf(coords, sizeof(coords), "%.4f&longitude=%.4f", (double)lat, (double)lon);
    std::string url = std::string("https://api.open-meteo.com/v1/forecast?latitude=") + coords +
                      "&current=temperature_2m,weather_code,apparent_temperature,relative_humidity_2m,"
                      "wind_speed_10m,wind_direction_10m,surface_pressure,visibility,pm2_5,us_aqi"
                      "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
                      "&forecast_days=4&timezone=Asia%2FShanghai";

    ESP_LOGI(TAG, "使用 Open-Meteo 获取天气 %s (%.4f, %.4f)", city_.c_str(), (double)lat, (double)lon);

    std::string body;
    if (!httpGetRaw(url, 10000, true, body)) {
        return false;
    }
    std::string json;
    if (!gunzip_to_string(body, json)) {
        json.swap(body);
    }

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Open-Meteo 响应解析失败 (head=%.80s)", json.c_str());
        return false;
    }

    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* temp = current ? cJSON_GetObjectItem(current, "temperature_2m") : nullptr;
    cJSON* wcode = current ? cJSON_GetObjectItem(current, "weather_code") : nullptr;
    if (temp == nullptr || !cJSON_IsNumber(temp)) {
        ESP_LOGW(TAG, "Open-Meteo 响应缺少 current.temperature_2m");
        cJSON_Delete(root);
        return false;
    }

    const int code = (wcode && cJSON_IsNumber(wcode)) ? wcode->valueint : -1;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", temp->valuedouble);

    d.city = city_.empty() ? WEATHER_CITY : city_;
    if (d.city.size() >= 3 && d.city.compare(d.city.size() - 3, 3, "市") == 0) {
        d.city.erase(d.city.size() - 3);   // "北京市" → "北京"
    }
    d.temp = buf;
    d.text = wmo_code_to_zh(code);
    d.update_time = "open-meteo";
    d.valid = true;

    // 逐项填充扩展维度（取不到就留空，UI 会显示 "--"）
    auto num = [](cJSON* obj, const char* key) -> cJSON* {
        cJSON* v = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
        return (v && cJSON_IsNumber(v)) ? v : nullptr;
    };
    cJSON* feels = num(current, "apparent_temperature");
    if (feels) { snprintf(buf, sizeof(buf), "%.0f", feels->valuedouble); d.feels_like = buf; }
    cJSON* hum = num(current, "relative_humidity_2m");
    if (hum) { snprintf(buf, sizeof(buf), "%.0f", hum->valuedouble); d.humidity = buf; }
    cJSON* wspd = num(current, "wind_speed_10m");
    cJSON* wdir = num(current, "wind_direction_10m");
    if (wdir) {
        const float deg = (float)wdir->valuedouble;
        const char* dirs[8] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
        d.wind_dir = dirs[(int)((deg + 22.5f) / 45.0f) % 8];
        d.wind_dir += "风";
    }
    if (wspd) {
        // km/h 转蒲福风级
        const float kmh = (float)wspd->valuedouble;
        const int scales[] = {0,1,2,3,4,5,6,7,8,9,10,11,12};
        const float bounds[] = {1,6,12,20,29,39,50,62,75,89,103,118,999};
        int lvl = 12;
        for (int i = 0; i < 13; ++i) { if (kmh < bounds[i]) { lvl = scales[i]; break; } }
        snprintf(buf, sizeof(buf), "%d", lvl); d.wind_scale = buf;
    }
    cJSON* pres = num(current, "surface_pressure");
    if (pres) { snprintf(buf, sizeof(buf), "%.0f", pres->valuedouble); d.pressure = buf; }
    cJSON* vis = num(current, "visibility");
    if (vis) { snprintf(buf, sizeof(buf), "%.0f", vis->valuedouble / 1000.0f); d.vis = buf; }
    cJSON* pm25 = num(current, "pm2_5");
    if (pm25) { snprintf(buf, sizeof(buf), "%.0f", pm25->valuedouble); d.pm25 = buf; }
    cJSON* aqi = num(current, "us_aqi");
    if (aqi) {
        snprintf(buf, sizeof(buf), "%.0f", aqi->valuedouble); d.aqi = buf;
        const int a = (int)aqi->valuedouble;
        d.aqi_category = a <= 50 ? "优" : (a <= 100 ? "良" : (a <= 150 ? "轻度污染" :
                        (a <= 200 ? "中度污染" : (a <= 300 ? "重度污染" : "严重污染"))));
    }

    // 日出日落 + 未来 3 天预报（daily 各字段是数组，索引 0=今天）
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    // ISO 8601 "2026-11-21T06:33" 取第 11 位起 5 字符 = "06:33"
    auto hhmm = [](cJSON* arr, int idx) -> std::string {
        cJSON* it = arr ? cJSON_GetArrayItem(arr, idx) : nullptr;
        if (!it || !cJSON_IsString(it) || !it->valuestring) return "";
        const char* s = it->valuestring;
        return strlen(s) >= 16 ? std::string(s + 11, 5) : "";
    };
    cJSON* sunrise_arr = daily ? cJSON_GetObjectItem(daily, "sunrise") : nullptr;
    cJSON* sunset_arr = daily ? cJSON_GetObjectItem(daily, "sunset") : nullptr;
    d.sunrise = hhmm(sunrise_arr, 0);
    d.sunset = hhmm(sunset_arr, 0);

    cJSON* dcodes = daily ? cJSON_GetObjectItem(daily, "weather_code") : nullptr;
    cJSON* dmaxs = daily ? cJSON_GetObjectItem(daily, "temperature_2m_max") : nullptr;
    cJSON* dmins = daily ? cJSON_GetObjectItem(daily, "temperature_2m_min") : nullptr;
    int day_count = 0;
    if (dmaxs && cJSON_IsArray(dmaxs)) day_count = cJSON_GetArraySize(dmaxs);
    if (dcodes && cJSON_IsArray(dcodes) && (int)cJSON_GetArraySize(dcodes) < day_count)
        day_count = cJSON_GetArraySize(dcodes);
    if (day_count > 3) day_count = 3;
    for (int i = 0; i < day_count; ++i) {
        cJSON* dc = cJSON_GetArrayItem(dcodes, i);
        cJSON* dhi = cJSON_GetArrayItem(dmaxs, i);
        cJSON* dlo = cJSON_GetArrayItem(dmins, i);
        if (dc && cJSON_IsNumber(dc)) d.daily[i].text_day = wmo_code_to_zh(dc->valueint);
        if (dhi && cJSON_IsNumber(dhi)) {
            snprintf(buf, sizeof(buf), "%.0f", dhi->valuedouble); d.daily[i].temp_max = buf;
        }
        if (dlo && cJSON_IsNumber(dlo)) {
            snprintf(buf, sizeof(buf), "%.0f", dlo->valuedouble); d.daily[i].temp_min = buf;
        }
        d.daily[i].sunrise = hhmm(sunrise_arr, i);
        d.daily[i].sunset = hhmm(sunset_arr, i);
    }
    d.daily_count = day_count;

    ESP_LOGI(TAG, "天气更新成功(Open-Meteo): %s %s %s°C (code=%d)",
             d.city.c_str(), d.text.c_str(),
             d.temp.c_str(), code);
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_ = d;
    }
    return true;
}

// 用 Open-Meteo 当前实况覆盖温度/体感/湿度：itboy 的 wendu 常停留在清晨旧值，
// 只覆盖这三项，其余维度保留 itboy（AQI/风力/日出日落/预报），避免 US AQI 标准差异。
// 任何一步失败都不改动已有数据（静默回退到 itboy 原值）。
bool WeatherManager::overlayRealtimeFromOpenMeteo() {
    const CityGeoParam* gp = city_geo_param(city_);
    const float lat = (gp != nullptr) ? gp->lat : WEATHER_LAT;
    const float lon = (gp != nullptr) ? gp->lon : WEATHER_LON;
    char coords[64];
    snprintf(coords, sizeof(coords), "%.4f&longitude=%.4f", (double)lat, (double)lon);
    // 只取实时值修正 itboy 的清晨脏 wendu；三天预报保留 itboy（与用户手机同源中国天气网）
    std::string url = std::string("https://api.open-meteo.com/v1/forecast?latitude=") + coords +
                      "&current=temperature_2m,apparent_temperature,relative_humidity_2m"
                      "&timezone=Asia%2FShanghai";

    std::string body;
    if (!httpGetRaw(url, 10000, true, body)) {
        ESP_LOGW(TAG, "Open-Meteo 实时覆盖请求失败，沿用 itboy 温度");
        return false;
    }
    std::string json;
    if (!gunzip_to_string(body, json)) {
        json.swap(body);
    }
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* t = current ? cJSON_GetObjectItem(current, "temperature_2m") : nullptr;
    cJSON* f = current ? cJSON_GetObjectItem(current, "apparent_temperature") : nullptr;
    cJSON* h = current ? cJSON_GetObjectItem(current, "relative_humidity_2m") : nullptr;
    cJSON* tm = current ? cJSON_GetObjectItem(current, "time") : nullptr;
    if (t == nullptr || !cJSON_IsNumber(t)) {
        cJSON_Delete(root);
        return false;
    }

    // 先算好字符串，再一次性加锁写入，避免持锁做解析
    char buf[24];
    std::string temp, feels, hum, upd;
    snprintf(buf, sizeof(buf), "%.0f", t->valuedouble); temp = buf;
    if (f && cJSON_IsNumber(f)) { snprintf(buf, sizeof(buf), "%.0f", f->valuedouble); feels = buf; }
    if (h && cJSON_IsNumber(h)) { snprintf(buf, sizeof(buf), "%.0f", h->valuedouble); hum = buf; }
    // Open-Meteo time 形如 "2026-11-21T11:00"，直接作为 update_time，详情页可显示更新时间
    if (tm && cJSON_IsString(tm) && tm->valuestring[0] != 0) {
        upd = tm->valuestring;
    }

    cJSON_Delete(root);

    {
        std::lock_guard<std::mutex> lk(mutex_);
        latest_data_.temp = temp;
        if (!feels.empty()) latest_data_.feels_like = feels;
        if (!hum.empty()) latest_data_.humidity = hum;
        if (!upd.empty()) latest_data_.update_time = upd;
    }
    ESP_LOGI(TAG, "Open-Meteo 实时覆盖: %s°C 体感%s°C 湿度%s%% (%s) 三天预报保留 itboy",
             temp.c_str(), feels.c_str(), hum.c_str(), upd.c_str());
    return true;
}

std::string WeatherManager::getWeatherSummary() {
    std::lock_guard<std::mutex> lk(mutex_);
    char buf[384];
    snprintf(buf, sizeof(buf),
             "city=%s source=%s data=%s %s %s°C humidity=%s aqi=%s/%s wind=%s%s "
             "pm25=%s sunrise=%s sunset=%s daily=%d",
             city_.c_str(), city_source_.c_str(),
             latest_data_.city.c_str(), latest_data_.text.c_str(), latest_data_.temp.c_str(),
             latest_data_.humidity.c_str(), latest_data_.aqi.c_str(),
             latest_data_.aqi_category.c_str(),
             latest_data_.wind_dir.c_str(), latest_data_.wind_scale.c_str(),
             latest_data_.pm25.c_str(), latest_data_.sunrise.c_str(),
             latest_data_.sunset.c_str(), latest_data_.daily_count);
    return std::string(buf);
}
