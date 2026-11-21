#include "local_mcp_bridge.h"
#include "config.h"
#include "settings.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>

static const char* TAG = "LocalMcpBridge";

#ifndef LOCAL_MCP_URL
#define LOCAL_MCP_URL ""
#endif

struct BridgeCtx {
    std::string data;
};

static esp_err_t bridge_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != nullptr && evt->data_len > 0) {
        auto* ctx = static_cast<BridgeCtx*>(evt->user_data);
        // 限制响应体最大 32KB，防止异常服务撑爆内存
        if (ctx->data.size() < 32768) {
            ctx->data.append(static_cast<const char*>(evt->data), evt->data_len);
        }
    }
    return ESP_OK;
}

LocalMcpBridge& LocalMcpBridge::GetInstance() {
    static LocalMcpBridge instance;
    return instance;
}

std::string LocalMcpBridge::GetUrl() {
    Settings settings("local_mcp", false);
    std::string url = settings.GetString("url", "");
    if (url.empty() && std::strlen(LOCAL_MCP_URL) > 0) {
        url = LOCAL_MCP_URL;
        SetUrl(url);
    }
    return url;
}

void LocalMcpBridge::SetUrl(const std::string& url) {
    Settings settings("local_mcp", true);
    settings.SetString("url", url);
}

bool LocalMcpBridge::Call(const std::string& tool, const std::string& args_json, std::string& out) {
    std::string url = GetUrl();
    if (url.empty()) {
        out = "本地 MCP 地址未配置，请先说：把本地服务地址设为 http://192.168.1.11:8123";
        return false;
    }
    // 只接受 http://，避免内网 TLS 证书问题
    if (url.rfind("http://", 0) != 0) {
        out = "本地 MCP 地址必须以 http:// 开头";
        return false;
    }

    // 组装请求体 {"tool":"...","args":{...}}
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tool", tool.c_str());
    cJSON* args = cJSON_Parse(args_json.c_str());
    if (args == nullptr || !cJSON_IsObject(args)) {
        cJSON_Delete(root);
        if (args) cJSON_Delete(args);
        out = "参数必须是 JSON 对象，例如 {\"filename\":\"报表.xlsx\",\"title\":\"标题\"}";
        return false;
    }
    cJSON_AddItemToObject(root, "args", args);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == nullptr) {
        out = "参数序列化失败";
        return false;
    }

    BridgeCtx ctx;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.event_handler = bridge_event_handler;
    config.user_data = &ctx;
    config.timeout_ms = 15000;  // 写 Excel 等稍慢操作留足时间

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        free(body);
        out = "HTTP 客户端初始化失败";
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, std::strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        out = std::string("请求本地服务失败，err=") + std::to_string(err);
        return false;
    }
    if (status != 200) {
        out = std::string("本地服务返回 HTTP ") + std::to_string(status) + "：" + ctx.data.substr(0, 300);
        return false;
    }

    // 解析 {"ok":bool,"result"/"error":"..."}
    cJSON* resp = cJSON_Parse(ctx.data.c_str());
    if (resp == nullptr) {
        out = "本地服务返回内容无法解析：" + ctx.data.substr(0, 200);
        return false;
    }
    cJSON* ok = cJSON_GetObjectItem(resp, "ok");
    cJSON* text = cJSON_GetObjectItem(resp, cJSON_IsTrue(ok) ? "result" : "error");
    bool success = cJSON_IsTrue(ok);
    if (cJSON_IsString(text) && text->valuestring != nullptr) {
        out = text->valuestring;
    } else {
        out = success ? "操作完成（无返回文本）" : "本地服务返回错误";
    }
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "桥接调用 %s -> %s", tool.c_str(), success ? "ok" : "fail");
    return success;
}
