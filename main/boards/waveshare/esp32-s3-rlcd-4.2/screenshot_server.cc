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
#include "managers/weather_manager.h"
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

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kShotPort;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 14;      // 调试端点已超过默认 8 个
    config.stack_size = 12288;         // /weather_set 内同步跑一次天气请求；截图也需较大栈
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败（端口 %d 被占用？）", kShotPort);
        s_server = nullptr;
        heap_caps_free(s_bmp);
        s_bmp = nullptr;
        return false;
    }
    httpd_register_uri_handler(s_server, &kShotUri);
    httpd_register_uri_handler(s_server, &kDbgUri);
    httpd_register_uri_handler(s_server, &kDitherUri);
    httpd_register_uri_handler(s_server, &kPageUri);
    httpd_register_uri_handler(s_server, &kCountdownUri);
    httpd_register_uri_handler(s_server, &kWeatherQueryUri);
    httpd_register_uri_handler(s_server, &kWeatherSetUri);
    httpd_register_uri_handler(s_server, &kWeatherDataUri);
    httpd_register_uri_handler(s_server, &kMusicPlayUri);
    httpd_register_uri_handler(s_server, &kMusicStopUri);
    httpd_register_uri_handler(s_server, &kMusicStateUri);
    ESP_LOGI(TAG, "截图服务已启动: http://<设备IP>:%d/shot.bmp", kShotPort);
    return true;
}
