#ifndef LOCAL_MCP_BRIDGE_H
#define LOCAL_MCP_BRIDGE_H

#include <string>

// 本地 MCP 桥接客户端：把云端 LLM 的工具调用经 ESP32 转发到局域网内的
// Python 服务（http://<PC>:8123/call），让 LLM 能在本机写文件/查状态。
class LocalMcpBridge {
public:
    static LocalMcpBridge& GetInstance();

    // 调用桥接端点。args_json 是工具参数的 JSON 对象字符串。
    // 成功返回 true，out 为服务返回的 result 文本；失败 out 为错误说明。
    bool Call(const std::string& tool, const std::string& args_json, std::string& out);

    // 地址读写（持久化在 NVS namespace "local_mcp", key "url"）
    std::string GetUrl();
    void SetUrl(const std::string& url);

private:
    LocalMcpBridge() = default;
};

#endif
