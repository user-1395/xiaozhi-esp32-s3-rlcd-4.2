// 屏幕截图 HTTP 服务
//
// 目的：RLCD 是 1-bit 单色屏，没法用摄像头/系统截图工具看画面，所以把显存导出成 BMP
// 走局域网 HTTP，主机侧 curl 一下就能看到设备真实画面（也方便回归对比 UI 改动）。
//
// 端点：GET /shot.bmp  → image/bmp，1bpp 黑白，尺寸 = 面板宽高
// 端口：8080（避开配网页面的 80 端口）

#include "screenshot_server.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>

#include "custom_lcd_display.h"
#include "application.h"
#include "sdcard_service.h"
#include "managers/weather_manager.h"
#include "managers/local_mcp_bridge.h"
#include <settings.h>   // P2 #14：从 NVS 读取可选鉴权 token

static const char *TAG = "Screenshot";

// httpd_query_key_value 不做 URL 解码，这里就地解开 %XX 和 +。
static void UrlDecodeInPlace(char *s) {
    if (s == nullptr) return;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    char *w = s;
    for (char *r = s; *r != '\0'; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit(static_cast<unsigned char>(r[1])) &&
                   isxdigit(static_cast<unsigned char>(r[2]))) {
            *w++ = static_cast<char>((hex(r[1]) << 4) | hex(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static httpd_handle_t s_server = nullptr;
static CustomLcdDisplay *s_display = nullptr;
static uint8_t *s_bmp = nullptr;
static size_t s_bmp_cap = 0;
static const int kShotPort = 8080;

// ---- P2 #14：可选访问鉴权 ----
// 默认不配置 token → 保持原有调试行为（本地截图脚本无需改动）；
// 配置 token（NVS 键 screenshot/token，或编译期 -DSCREENSHOT_TOKEN="..."）后，
// 所有端点要求 URL 带 ?token=xxx 才放行。
static std::string s_token;

static void LoadScreenshotToken() {
    s_token.clear();
    // 1) NVS 优先（设备上可写，无需重编译）
    Settings settings("screenshot", false);
    std::string t = settings.GetString("token", "");
    if (!t.empty()) {
        s_token = t;
        return;
    }
    // 2) 编译期 token
#ifdef SCREENSHOT_TOKEN
    if (strlen(SCREENSHOT_TOKEN) > 0) {
        s_token = SCREENSHOT_TOKEN;
    }
#endif
}

static bool TokenEquals(const std::string &expected, const char *got) {
    if (got == nullptr) {
        return false;
    }
    const size_t n = strlen(got);
    if (expected.size() != n) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(expected[i] ^ got[i]);
    }
    return diff == 0;
}

// 返回 true 表示放行；false 时已发送 401
static bool CheckAuth(httpd_req_t *req) {
    if (s_token.empty()) {
        return true;   // 未配置 token → 调试开放（原有行为）
    }
    char query[256] = {0};
    char value[128] = {0};
    bool ok = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "token", value, sizeof(value)) == ESP_OK) {
        ok = TokenEquals(s_token, value);
    }
    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, "unauthorized: ?token= required\n", HTTPD_RESP_USE_STRLEN);
    }
    return ok;
}

static esp_err_t ShotBmpHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr || s_bmp == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    size_t len = 0;
    if (!s_display->CaptureScreenBmp(s_bmp, s_bmp_cap, &len)) {
        ESP_LOGE(TAG, "截图失败");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)s_bmp, (ssize_t)len);
    ESP_LOGI(TAG, "已发送截图 %u 字节", (unsigned)len);
    return err;
}

// 调试端点：把关键控件状态以文本返回，排查「控件不显示」这类问题
static esp_err_t DbgHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char head[64];
    int hn = snprintf(head, sizeof(head), "dither_mode=%d thr=%d\n",
                      s_display->GetDitherMode(), s_display->GetThreshold());
    std::string info = std::string(head, (size_t)(hn > 0 ? hn : 0)) + s_display->GetUiDebugInfo();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, info.c_str(), (ssize_t)info.size());
}

// 在线切换 1-bit 抖动模式：/dither?mode=0|1|2（不带参数只报告当前值），便于 A/B 对比观感
static esp_err_t DitherHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char value[8] = {0};
    int mode = -1;
    int thr = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "mode", value, sizeof(value)) == ESP_OK) {
            mode = atoi(value);
        }
        if (httpd_query_key_value(query, "thr", value, sizeof(value)) == ESP_OK) {
            thr = atoi(value);
        }
    }
    if (mode >= 0 && mode <= 4) {
        s_display->SetDitherMode(mode);
        s_display->RedrawAll();
    }
    if (thr >= 16 && thr <= 240) {
        s_display->SetThreshold(thr);
        s_display->RedrawAll();
    }
    char out[128];
    int n = snprintf(out, sizeof(out),
                     "dither_mode=%d thr=%d (0=threshold 1=bayer4 2=bayer8 3/4=band-dither)\n",
                     s_display->GetDitherMode(), s_display->GetThreshold());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

// 在线切页：/page?p=weather|detail|music|toggle（不带参数只报告当前页），便于远程看另一页的画面
static esp_err_t PageHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "p", value, sizeof(value)) == ESP_OK) {
        if (strcmp(value, "music") == 0) {
            s_display->SwitchToMusicPage();
        } else if (strcmp(value, "weather") == 0) {
            s_display->SwitchToWeatherPage();
        } else if (strcmp(value, "detail") == 0) {
            s_display->SwitchToWeatherDetailPage();
        } else if (strcmp(value, "toggle") == 0) {
            if (s_display->IsMusicMode()) {
                s_display->SwitchToWeatherPage();
            } else {
                s_display->SwitchToMusicPage();
            }
        }
    }
    const char *page = s_display->IsMusicMode()    ? "music"
                       : s_display->IsDetailMode() ? "detail"
                                                   : "weather";
    char out[64];
    int n = snprintf(out, sizeof(out), "page=%s\n", page);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}


// 调试端点：/countdown?action=start|pause|resume|cancel|status&sec=300
// 便于无语音环境下远程验证倒计时页与状态机
static esp_err_t CountdownHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[96] = {0};
    char action[16] = {0};
    char secbuf[16] = {0};
    int seconds = 60;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK) {
            strcpy(action, "status");
        }
        if (httpd_query_key_value(query, "sec", secbuf, sizeof(secbuf)) == ESP_OK) {
            seconds = atoi(secbuf);
        }
    } else {
        strcpy(action, "status");
    }

    bool ok = true;
    if (strcmp(action, "start") == 0) {
        ok = s_display->StartCountdown(seconds);
    } else if (strcmp(action, "pause") == 0) {
        ok = s_display->PauseCountdown();
    } else if (strcmp(action, "resume") == 0) {
        ok = s_display->ResumeCountdown();
    } else if (strcmp(action, "cancel") == 0) {
        ok = s_display->CancelCountdown();
    }

    char out[160];
    int n = snprintf(out, sizeof(out),
                     "action=%s ok=%d active=%d remaining=%d page=%s\n",
                     action, ok ? 1 : 0, s_display->IsCountdownActive() ? 1 : 0,
                     s_display->GetCountdownRemainingSec(),
                     s_display->IsCountdownMode() ? "countdown" : "other");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

// 调试端点：/weather_query?ms=15000 → 模拟“语音查询天气”切详情页并在 ms 后自动返回
static esp_err_t WeatherQueryHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char msbuf[16] = {0};
    int ms = 15000;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "ms", msbuf, sizeof(msbuf)) == ESP_OK) {
        ms = atoi(msbuf);
        if (ms < 1000) ms = 1000;
        if (ms > 60000) ms = 60000;
    }
    s_display->ShowWeatherDetailForQuery((uint32_t)ms);
    char out[96];
    int n = snprintf(out, sizeof(out), "weather detail shown, auto_return_ms=%d\n", ms);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}


// 调试端点：/weather_set?city=北京&adm= → 切换天气城市并立即刷新数据源
static esp_err_t WeatherSetHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    char query[96] = {0};
    char citybuf[40] = {0};
    char admbuf[40] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "city", citybuf, sizeof(citybuf)) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "missing city\n", HTTPD_RESP_USE_STRLEN);
    }
    httpd_query_key_value(query, "adm", admbuf, sizeof(admbuf));
    UrlDecodeInPlace(citybuf);
    UrlDecodeInPlace(admbuf);

    WeatherManager& wm = WeatherManager::getInstance();
    wm.SetCity(citybuf, admbuf);
    bool ok = wm.update();
    std::string summary = wm.getWeatherSummary();
    char out[160];
    int n = snprintf(out, sizeof(out), "ok=%d %s\n", ok ? 1 : 0, summary.c_str());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

// 调试端点：/weather_data → 查看当前天气缓存（验证数据源是否跟随城市）
static esp_err_t WeatherDataHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    WeatherManager& wm = WeatherManager::getInstance();
    std::string summary = wm.getWeatherSummary();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, summary.c_str(), summary.length());
}


// 调试端点：/music_play?url=<encoded>&title=..&artist=.. → 模拟语音播放，验证自动切页/停留
static esp_err_t MusicPlayHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    char query[512] = {0};
    char url[384] = {0};
    char title[64] = {0};
    char artist[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "url", url, sizeof(url)) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "missing url\n", HTTPD_RESP_USE_STRLEN);
    }
    httpd_query_key_value(query, "title", title, sizeof(title));
    httpd_query_key_value(query, "artist", artist, sizeof(artist));
    UrlDecodeInPlace(url);
    UrlDecodeInPlace(title);
    UrlDecodeInPlace(artist);
    Application::GetInstance().PlayMusicFromUrl(url, title, artist, "");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "music play requested\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t MusicStopHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    Application::GetInstance().StopMusicPlayback();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "music stopped\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t MusicStateHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    char out[96];
    int n = snprintf(out, sizeof(out), "playing=%d page=%s\n",
                     Application::GetInstance().IsMusicPlaying() ? 1 : 0,
                     (s_display != nullptr && s_display->IsMusicMode()) ? "music" : "other");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

// ---------- SD 卡文件服务 ----------
// GET  /sd/status             → 卡状态/容量
// GET  /sd/list?path=/sdcard  → 列目录（JSON 数组）
// GET  /sd/read?path=/sdcard/x.txt → 读文件（二进制原样返回）
// POST /sd/write?path=/sdcard/x.txt (body=内容) → 写文件
static esp_err_t SdStatusHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) return ESP_OK;
    auto &sd = SdcardService::GetInstance();
    std::string out = std::string("ready=") + (sd.IsReady() ? "1" : "0") +
                      "\ninfo=" + sd.CardInfo() + "\n";
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.c_str(), (ssize_t)out.size());
}

static esp_err_t SdListHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) return ESP_OK;
    auto &sd = SdcardService::GetInstance();
    if (!sd.IsReady()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd not ready");

    char query[512] = {0};
    char path[256] = "/sdcard";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[256] = {0};
        if (httpd_query_key_value(query, "path", val, sizeof(val)) == ESP_OK) {
            UrlDecodeInPlace(val);
            snprintf(path, sizeof(path), "%s", val);
        }
    }
    std::vector<std::string> entries;
    int ret = sd.ListDir(path, entries);
    if (ret != 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "open dir failed");

    std::string json = "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) json += ",";
        std::string e;
        for (char c : entries[i]) {
            if (c == '"' || c == '\\') e += '\\';
            e += c;
        }
        json += "\"" + e + "\"";
    }
    json += "]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), (ssize_t)json.size());
}

static esp_err_t SdReadHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) return ESP_OK;
    auto &sd = SdcardService::GetInstance();
    if (!sd.IsReady()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd not ready");

    char query[512] = {0};
    char path[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[256] = {0};
        if (httpd_query_key_value(query, "path", val, sizeof(val)) == ESP_OK) {
            UrlDecodeInPlace(val);
            snprintf(path, sizeof(path), "%s", val);
        }
    }
    if (path[0] == '\0') return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path required");

    std::string data;
    int len = sd.ReadFile(path, data);
    if (len < 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "read failed");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, data.data(), (ssize_t)data.size());
}

static esp_err_t SdWriteHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) return ESP_OK;
    auto &sd = SdcardService::GetInstance();
    if (!sd.IsReady()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd not ready");

    char query[512] = {0};
    char path[256] = {0};
    bool append = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[256] = {0};
        if (httpd_query_key_value(query, "path", val, sizeof(val)) == ESP_OK) {
            UrlDecodeInPlace(val);
            snprintf(path, sizeof(path), "%s", val);
        }
        char ap[8] = {0};
        if (httpd_query_key_value(query, "append", ap, sizeof(ap)) == ESP_OK && strcmp(ap, "1") == 0) {
            append = true;
        }
    }
    if (path[0] == '\0') return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path required");

    size_t total = (size_t)req->content_len;
    char *buf = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, (size_t)(total - got));
        if (r <= 0) { heap_caps_free(buf); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed"); }
        got += (size_t)r;
    }
    buf[got] = '\0';

    int ret = append ? sd.AppendFile(path, buf, got) : sd.WriteFile(path, buf, got);
    heap_caps_free(buf);
    if (ret < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");

    char out[64];
    int n = snprintf(out, sizeof(out), "ok bytes=%d\n", (int)got);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, out, (ssize_t)n);
}

static const httpd_uri_t kSdStatusUri = {
    .uri = "/sd/status", .method = HTTP_GET, .handler = SdStatusHandler, .user_ctx = nullptr,
};
static const httpd_uri_t kSdListUri = {
    .uri = "/sd/list", .method = HTTP_GET, .handler = SdListHandler, .user_ctx = nullptr,
};
static const httpd_uri_t kSdReadUri = {
    .uri = "/sd/read", .method = HTTP_GET, .handler = SdReadHandler, .user_ctx = nullptr,
};
static const httpd_uri_t kSdWriteUri = {
    .uri = "/sd/write", .method = HTTP_POST, .handler = SdWriteHandler, .user_ctx = nullptr,
};

static const httpd_uri_t kShotUri = {
    .uri = "/shot.bmp",
    .method = HTTP_GET,
    .handler = ShotBmpHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kDbgUri = {
    .uri = "/dbg",
    .method = HTTP_GET,
    .handler = DbgHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kPageUri = {
    .uri = "/page",
    .method = HTTP_GET,
    .handler = PageHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kDitherUri = {
    .uri = "/dither",
    .method = HTTP_GET,
    .handler = DitherHandler,
    .user_ctx = nullptr,
};

// v10: 软件反色切换（深底/浅底）。面板极性恒 0x20，送显位流取反，
// 避免硬件 0x21 反色导致的左上角边缘像素闪烁。
static esp_err_t InvertHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr || s_display->rlcd() == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char value[8] = {0};
    int on = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "on", value, sizeof(value)) == ESP_OK) {
        on = atoi(value);
    }
    if (on == 0 || on == 1) {
        s_display->SetInversion(on == 1);
    }
    char out[128];
    int n = snprintf(out, sizeof(out),
                     "inversion=%s (当前=%s; on=1 深底白字 / on=0 浅底黑字, 软件取反无闪烁)\n",
                     on < 0 ? "unchanged" : (on ? "on" : "off"),
                     s_display->inversion() ? "on" : "off");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

static const httpd_uri_t kInvertUri = {
    .uri = "/invert",
    .method = HTTP_GET,
    .handler = InvertHandler,
    .user_ctx = nullptr,
};

// v11: VCOM 电压运行时校准。?level=0x0C..0x16 设置，无参数读当前值。
static esp_err_t VcomHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr || s_display->rlcd() == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[64] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "level", value, sizeof(value)) == ESP_OK) {
        // 支持 0x 前缀十六进制或十进制
        char *endp = nullptr;
        long lv = strtol(value, &endp, 0);
        if (endp != value && lv >= 0 && lv <= 0x1F) {
            s_display->SetVcom((uint8_t)lv);
        }
    }
    char out[96];
    int n = snprintf(out, sizeof(out), "vcom=0x%02X (推荐 0x0D~0x15, 默认 0x11)\n",
                     s_display->vcomLevel());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

static const httpd_uri_t kVcomUri = {
    .uri = "/vcom",
    .method = HTTP_GET,
    .handler = VcomHandler,
    .user_ctx = nullptr,
};

// v11: VCOM 校准网页面板——每个候选值一个按钮，点一下即设置并回读，
// 配合 /invert?on=1 深色模式肉眼观察左上角是否还闪。
static const char kVcomTestHtml[] =
    "<!doctype html><html><head><meta charset='utf-8'><title>VCOM 校准</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:20px;background:#fafafa;color:#222}"
    "h1{font-size:20px}.row{margin:8px 0}a.btn{display:inline-block;padding:8px 14px;margin:3px;"
    "background:#fff;border:2px solid #444;border-radius:6px;text-decoration:none;color:#111;font-weight:bold}"
    "a.btn.cur{background:#444;color:#fff}small{color:#666}</style></head><body>"
    "<h1>VCOM 电压校准（找深色闪烁最小的电压）</h1>"
    "<p>先在设备上切到<strong>深色</strong>（地址栏打开 <code>/invert?on=1</code>），然后逐个点下面的电压值，"
    "每点一次看屏幕<strong>左上角那排像素</strong>还闪不闪，找到不闪的值记下来。</p>"
    "<p>当前 VCOM: <strong id='cur'>--</strong> &nbsp; <small>默认 0x11，推荐范围 0x0D~0x15</small></p>"
    "<div class='row' id='low'></div><div class='row' id='mid'></div><div class='row' id='high'></div>"
    "<script>"
    "var vals=[0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,0x14,0x15,0x16];"
    "function build(){['low','mid','high'].forEach(function(id,i){\n"
    "  var box=document.getElementById(id);box.innerHTML='';\n"
    "  vals.slice(i*4,i*4+4).forEach(function(v){\n"
    "    var a=document.createElement('a');a.className='btn'+(v===cur?' cur':'');\n"
    "    a.href='javascript:void(0)';a.textContent='0x'+v.toString(16).toUpperCase().padStart(2,'0');\n"
    "    a.onclick=function(){fetch('/vcom?level='+v).then(function(r){return r.text()}).then(function(t){cur=v;build();});};"
    "    box.appendChild(a);});});}\n"
    "var cur=0x11;"
    "fetch('/vcom').then(function(r){return r.text()}).then(function(t){"
    "  var m=t.match(/0x([0-9A-Fa-f]{2})/);if(m){cur=parseInt(m[1],16);}"
    "  document.getElementById('cur').textContent='0x'+cur.toString(16).toUpperCase().padStart(2,'0');build();});"
    "</script></body></html>";

static esp_err_t VcomTestHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kVcomTestHtml, strlen(kVcomTestHtml));
}

static const httpd_uri_t kVcomTestUri = {
    .uri = "/vcomtest",
    .method = HTTP_GET,
    .handler = VcomTestHandler,
    .user_ctx = nullptr,
};

// v12: 持续刷新开关（对齐微雪官方 U8G2 例程 74FPS 快速刷新，深底不闪）。
// GET /refresh?on=1|0&ms=13   on=1 开 / on=0 关 / ms=间隔(1~200)；无参数只报告状态。
static esp_err_t RefreshHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    if (s_display == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not ready");
    }
    char query[96] = {0};
    char onbuf[8] = {0};
    char msbuf[8] = {0};
    int on = -1;
    int ms = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "on", onbuf, sizeof(onbuf)) == ESP_OK) {
            on = atoi(onbuf);
        }
        if (httpd_query_key_value(query, "ms", msbuf, sizeof(msbuf)) == ESP_OK) {
            ms = atoi(msbuf);
        }
    }
    if (on == 0 || on == 1) {
        s_display->SetRefresh(on == 1, ms);
    } else if (ms >= 1 && ms <= 200) {
        s_display->SetRefresh(s_display->refreshEnabled(), ms);
    }
    char out[160];
    int n = snprintf(out, sizeof(out),
                     "refresh=%s interval=%dms (官方 74FPS≈13ms; on=1开 on=0关 ms=间隔)\n",
                     s_display->refreshEnabled() ? "on" : "off",
                     s_display->refreshIntervalMs());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

static const httpd_uri_t kRefreshUri = {
    .uri = "/refresh",
    .method = HTTP_GET,
    .handler = RefreshHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kMusicPlayUri = {
    .uri = "/music_play",
    .method = HTTP_GET,
    .handler = MusicPlayHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kMusicStopUri = {
    .uri = "/music_stop",
    .method = HTTP_GET,
    .handler = MusicStopHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kMusicStateUri = {
    .uri = "/music_state",
    .method = HTTP_GET,
    .handler = MusicStateHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kCountdownUri = {
    .uri = "/countdown",
    .method = HTTP_GET,
    .handler = CountdownHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kWeatherQueryUri = {
    .uri = "/weather_query",
    .method = HTTP_GET,
    .handler = WeatherQueryHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kWeatherSetUri = {
    .uri = "/weather_set",
    .method = HTTP_GET,
    .handler = WeatherSetHandler,
    .user_ctx = nullptr,
};

static const httpd_uri_t kWeatherDataUri = {
    .uri = "/weather_data",
    .method = HTTP_GET,
    .handler = WeatherDataHandler,
    .user_ctx = nullptr,
};

// 调试端点：/weather_api_set?host=<api_host>&key=<api_key> → 把和风凭证写进 NVS（热配置，无需重刷固件）
// 写完后清 LocationID 缓存并立即 update() 验证；凭证只在设备 NVS，不进源码/仓库
static esp_err_t WeatherApiSetHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    char query[384] = {0};
    char hostbuf[128] = {0};
    char keybuf[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "host", hostbuf, sizeof(hostbuf)) != ESP_OK ||
        httpd_query_key_value(query, "key", keybuf, sizeof(keybuf)) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "missing host or key\n", HTTPD_RESP_USE_STRLEN);
    }
    UrlDecodeInPlace(hostbuf);
    UrlDecodeInPlace(keybuf);
    {
        Settings wr("weather", true);
        wr.SetString("api_host", hostbuf);
        wr.SetString("api_key", keybuf);
        wr.SetString("loc_id", "");   // 换凭证后强制重新 geo 定位
        wr.SetString("loc_name", "");
        wr.SetString("loc_coord", "");
    }
    WeatherManager& wm = WeatherManager::getInstance();
    wm.loadConfigFromNvs();            // 让新凭证立即生效
    bool ok = wm.update();
    char out[96];
    int n = snprintf(out, sizeof(out), "ok=%d qw=%s\n", ok ? 1 : 0, wm.hasQWeather() ? "yes" : "no");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, n);
}

static const httpd_uri_t kWeatherApiSetUri = {
    .uri = "/weather_api_set",
    .method = HTTP_GET,
    .handler = WeatherApiSetHandler,
    .user_ctx = nullptr,
};

// 调试端点：直接触发本地 MCP 桥接，验证「设备 → PC」链路。
// GET /local_call?tool=create_excel&args=<urlencoded JSON>
static esp_err_t LocalCallHandler(httpd_req_t *req) {
    if (!CheckAuth(req)) {
        return ESP_OK;
    }
    // 用 std::string 堆分配，避免大 char 数组触发 -Werror=frame-larger-than=1536
    std::string query(1024, '\0');
    std::string toolbuf(64, '\0');
    std::string argsbuf(768, '\0');
    if (httpd_req_get_url_query_str(req, &query[0], query.size()) != ESP_OK ||
        httpd_query_key_value(query.c_str(), "tool", &toolbuf[0], toolbuf.size()) != ESP_OK ||
        httpd_query_key_value(query.c_str(), "args", &argsbuf[0], argsbuf.size()) != ESP_OK) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "missing tool or args\n", HTTPD_RESP_USE_STRLEN);
    }
    toolbuf.resize(std::strlen(toolbuf.c_str()));
    argsbuf.resize(std::strlen(argsbuf.c_str()));
    UrlDecodeInPlace(&toolbuf[0]);
    UrlDecodeInPlace(&argsbuf[0]);
    toolbuf.resize(std::strlen(toolbuf.c_str()));
    argsbuf.resize(std::strlen(argsbuf.c_str()));
    std::string out;
    bool ok = LocalMcpBridge::GetInstance().Call(toolbuf, argsbuf, out);
    std::string resp = std::string("ok=") + (ok ? "1" : "0") + "\n" + out + "\n";
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, resp.c_str(), resp.size());
}

static const httpd_uri_t kLocalCallUri = {
    .uri = "/local_call",
    .method = HTTP_GET,
    .handler = LocalCallHandler,
    .user_ctx = nullptr,
};

bool ScreenshotServerStart(CustomLcdDisplay *display) {
    if (s_server != nullptr) {
        return true;
    }
    if (display == nullptr) {
        return false;
    }
    LoadScreenshotToken();
    ESP_LOGI(TAG, "截图服务鉴权: %s", s_token.empty() ? "关闭(调试开放)" : "已启用(?token=)");
    RlcdDriver *drv = display->rlcd();
    const int w = (drv != nullptr) ? drv->width() : 400;
    const int h = (drv != nullptr) ? drv->height() : 300;
    s_bmp_cap = 62 + (size_t)(((w + 31) / 32) * 4) * (size_t)h;
    s_bmp = (uint8_t *)heap_caps_malloc(s_bmp_cap, MALLOC_CAP_SPIRAM);
    if (s_bmp == nullptr) {
        ESP_LOGE(TAG, "截图缓冲区分配失败 (%u 字节)", (unsigned)s_bmp_cap);
        return false;
    }
    s_display = display;

    // 默认保持浅色背景（不闪、省电）。深色闪烁排查已结束，属面板边缘特性。
    // 需要深色时可手动 /invert?on=1（诊断端点保留）。
    ESP_LOGI(TAG, "默认浅色背景");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kShotPort;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;      // 调试端点 + SD 卡 4 端点
    config.stack_size = 12288;         // /weather_set 内同步跑一次天气请求；截图也需较大栈
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（端口 %d 被占用？）", kShotPort);
        s_server = nullptr;
        heap_caps_free(s_bmp);
        s_bmp = nullptr;
        return false;
    }
    httpd_register_uri_handler(s_server, &kSdStatusUri);
    httpd_register_uri_handler(s_server, &kSdListUri);
    httpd_register_uri_handler(s_server, &kSdReadUri);
    httpd_register_uri_handler(s_server, &kSdWriteUri);
    httpd_register_uri_handler(s_server, &kShotUri);
    httpd_register_uri_handler(s_server, &kDbgUri);
    httpd_register_uri_handler(s_server, &kDitherUri);
    httpd_register_uri_handler(s_server, &kInvertUri);
    httpd_register_uri_handler(s_server, &kVcomUri);
    httpd_register_uri_handler(s_server, &kVcomTestUri);
    httpd_register_uri_handler(s_server, &kRefreshUri);
    httpd_register_uri_handler(s_server, &kPageUri);
    httpd_register_uri_handler(s_server, &kCountdownUri);
    httpd_register_uri_handler(s_server, &kWeatherQueryUri);
    httpd_register_uri_handler(s_server, &kWeatherSetUri);
    httpd_register_uri_handler(s_server, &kWeatherDataUri);
    httpd_register_uri_handler(s_server, &kWeatherApiSetUri);
    httpd_register_uri_handler(s_server, &kMusicPlayUri);
    httpd_register_uri_handler(s_server, &kMusicStopUri);
    httpd_register_uri_handler(s_server, &kMusicStateUri);
    httpd_register_uri_handler(s_server, &kLocalCallUri);
    ESP_LOGI(TAG, "截图服务已启动: http://<设备IP>:%d/shot.bmp", kShotPort);
    return true;
}
