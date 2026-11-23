#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <atomic>
#include <cstddef>
#include <driver/gpio.h>
#include "config.h"
#include "lcd_display.h"

// 统一屏幕尺寸/方向宏，供各 UI 页面使用（由 config.h 的 RLCD_PORTRAIT 决定）
#define UI_SCR_W   RLCD_WIDTH
#define UI_SCR_H   RLCD_HEIGHT
#define UI_PORTRAIT (RLCD_HEIGHT > RLCD_WIDTH)
#include "rlcd_driver.h"
#include "ui_limits.h"   // P2 #26：待办抓取/显示上限共享常量
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"

// 天气站 + AI 混合显示
// 
// 屏幕布局 (400x300, 1-bit 单色 RLCD)，按微雪成品界面实测比例复刻：
// 环境 32.1°C 39%        十月初八 ╭─WiFi─▭─99%─╮
// ┌─────────────────┐ ┌─────────────┐
// │ 10月2日 星期五   │ │ 待办    ☑ ★ │
// │ ██ 17:18 ██(反色)│ │ ─────────── │
// │ 成都 晴间多云 23°C│ │ ★ 抢火车票  │
// ├─────────────────┤ │ ★ 记得吃饭  │
// │ AI 卡(199x100)  │ │             │
// └─────────────────┘ └─────────────┘
//   时钟卡 199x120      待办卡 173x249
// 顶栏浮在最上方：左「环境 温度 湿度」，右农历（14px）+ WiFi/电池胶囊
// 时间行是反色块（白字黑底），横跨时钟卡内宽
// 竖屏 300x400：时钟卡 → 待办卡 → AI 卡 整宽纵向堆叠
//
// 代码拆分为多个文件：
//   rlcd_driver.h/cc      - RLCD 硬件驱动层（SPI、像素映射、初始化命令）
//   weather_ui.cc          - 天气站 UI 布局（SetupWeatherUI）
//   music_ui.cc            - 音乐页 UI 布局（SetupMusicUI）
//   data_update_task.cc    - 后台数据更新任务（时间/天气/传感器/电池/WiFi/AI状态）
//   custom_lcd_display.cc  - 核心类（构造/析构/AI适配/备忘录/基类重写）
class CustomLcdDisplay : public LcdDisplay {
private:
    enum DisplayMode {
        MODE_WEATHER = 0,
        MODE_MUSIC = 1,
        MODE_DETAIL = 2,   // 天气详情页（完整天气维度）
        MODE_COUNTDOWN = 3, // 独立倒计时页（仅黑底白字剩余时间）
    };
    DisplayMode display_mode_ = MODE_WEATHER;

    // RLCD 硬件驱动（独立模块，负责 SPI 通信和像素操作）
    RlcdDriver *rlcd_ = nullptr;

    // ===== 1-bit 脏区局部刷新状态 =====
    // 一帧内可能多次 flush，先按 x 条带累积，帧末（flush_is_last）统一提交
    struct FlushRange {
        int x1;
        int x2;
    };
    static constexpr int kMaxFlushRanges = 6;   // 超过就退化为整帧
    static constexpr int kFlushMergeGap = 8;    // 相邻条带间隙小于该值则合并
    FlushRange flush_ranges_[kMaxFlushRanges] = {};
    int flush_range_count_ = 0;
    bool flush_force_full_ = false;
    bool flush_first_frame_ = true;   // 首帧整屏刷新，保证面板与影子缓冲一致

    // 刷新统计（帧数 / 整帧 / 局部 / 跳过 / 累计字节 / 累计传输耗时）
    uint32_t stat_frames_ = 0;
    uint32_t stat_full_ = 0;
    uint32_t stat_partial_ = 0;
    uint32_t stat_skip_ = 0;
    uint64_t stat_bytes_ = 0;
    uint64_t stat_transfer_us_ = 0;

    void FlushAddRange(int x1, int x2);   // 累积/合并脏区条带
    void FlushCommit();                   // 帧末提交：整帧或逐条带局部刷新 + 统计

    // 1-bit 二值化参数（/dither 可在线修改；LVGL flush 任务与 HTTP 任务并发读写）
    std::atomic<int> dither_mode_{0};     // 0=硬阈值 1=Bayer4 2=Bayer8 3/4=带状
    std::atomic<int> threshold_{112};     // 模式 0 的阈值
    lv_obj_t *weather_page_ = nullptr;
    lv_obj_t *music_page_ = nullptr;
    lv_obj_t *countdown_page_ = nullptr;
    lv_obj_t *countdown_time_label_ = nullptr;   // 倒计时页唯一控件：剩余时间 MM:SS

    // ===== 天气站 UI 组件 =====
    // 状态栏（右上角浮动胶囊）
    lv_obj_t *sensor_label_ = nullptr;      // 左上角温湿度标签
    
    // 时钟卡片（左上/竖屏顶部）
    lv_obj_t *day_label_ = nullptr;         // 日期行 "10月2日 星期五"
    lv_obj_t *time_digit_label_[4] = {nullptr, nullptr, nullptr, nullptr};  // 翻页时钟 4 位数字（时十/时个/分十/分个）
    lv_obj_t *lunar_label_ = nullptr;       // 农历/节气/节日 "十月初七 小雪"
    lv_obj_t *weather_label_ = nullptr;     // 天气行 "成都 晴 23°C"
    
    // AI 对话卡片（左下）
    lv_obj_t *chat_card_ = nullptr;         // AI 卡片容器
    lv_obj_t *chat_status_label_ = nullptr; // AI 对话文字（右侧）
    lv_obj_t *emotion_label_ = nullptr;     // 表情文字（左侧下方）
    lv_obj_t *emotion_img_ = nullptr;       // 表情图片（左侧上方，小智自带 emoji）
    lv_obj_t *ai_face_ = nullptr;           // 内置绘制的猫脸（资源包无 emoji 时兜底显示）

    // 待办卡片（右侧整高）
    lv_obj_t *memo_rows_ = nullptr;           // 待办列表容器（每项 = ★+时间 一行 / 事项 一行）
    int memo_rows_w_ = 160;                   // 待办行可用宽度（布局时确定）
    lv_point_precise_t memo_dash_pts_[kTodoDisplayMaxItems][2] = {};  // 项间虚线端点（lv_line 不拷贝点数组，必须常驻）
    lv_obj_t *music_title_label_ = nullptr;   // 音乐页面：歌名
    lv_obj_t *music_artist_label_ = nullptr;  // 音乐页面：歌手
    lv_obj_t *music_lyric_prev_label_ = nullptr;  // 音乐页面：上一句歌词（淡色）
    lv_obj_t *music_lyric_label_ = nullptr;       // 音乐页面：当前歌词（醒目）
    lv_obj_t *music_lyric_next_label_ = nullptr;  // 音乐页面：下一句歌词（淡色）
    lv_obj_t *music_progress_bar_ = nullptr;  // 音乐页面：进度条
    lv_obj_t *music_progress_label_ = nullptr; // 音乐页面：进度文本
    lv_obj_t *music_time_label_ = nullptr;    // 音乐页面：左上角时钟
    lv_obj_t *music_sensor_label_ = nullptr;  // 音乐页面：左上角温湿度
    lv_obj_t *music_chat_status_label_ = nullptr; // 音乐页面：小智状态文案
    lv_obj_t *music_emotion_label_ = nullptr; // 音乐页面：小智情绪标签
    lv_obj_t *music_emotion_img_ = nullptr;   // 音乐页面：小智表情图片（和天气页 emotion_img_ 对应）
    lv_obj_t *music_ai_face_ = nullptr;       // 音乐页面：内置猫脸（无 emoji 素材时兜底）

    // ===== 天气详情页（detail_ui.cc）=====
    // 信息层级：当前天气 hero（大温度 + 天气徽标 + 今日温差）> 体感/湿度/风 > 指标条 > 三日预报 > 日出日落/预警
    lv_obj_t *detail_page_ = nullptr;
    lv_obj_t *d_city_label_ = nullptr;        // 标题行：城市名
    lv_obj_t *d_update_label_ = nullptr;      // 标题行最右：更新时间
    lv_obj_t *d_hero_cond_badge_ = nullptr;   // 天气徽标底（黑底白字）
    lv_obj_t *d_hero_cond_label_ = nullptr;   // 天气现象：阴/多云/小雨
    lv_obj_t *d_hero_temp_label_ = nullptr;   // 当前温度数字（大字）
    lv_obj_t *d_hero_unit_label_ = nullptr;   // 温度单位 °C
    lv_obj_t *d_hero_range_label_ = nullptr;  // 今日 10~17°C
    lv_obj_t *d_hero_extra_value_[3] = {nullptr, nullptr, nullptr};  // 体感/湿度/风
    lv_obj_t *d_metric_value_[3] = {nullptr, nullptr, nullptr};      // AQI/能见度/气压
    lv_obj_t *d_metric_sub_[3] = {nullptr, nullptr, nullptr};        // AQI 的 PM2.5，其余留空
    lv_obj_t *d_fc_day_[3] = {nullptr, nullptr, nullptr};            // 今天/明天/后天
    lv_obj_t *d_fc_text_[3] = {nullptr, nullptr, nullptr};           // 白天天气
    lv_obj_t *d_fc_high_[3] = {nullptr, nullptr, nullptr};           // 最高温（大字）
    lv_obj_t *d_fc_low_[3] = {nullptr, nullptr, nullptr};            // 最低温（小字 /10°）
    lv_obj_t *d_sun_label_ = nullptr;         // 日出日落
    lv_obj_t *d_warn_card_ = nullptr;         // 预警条底（有预警时反色）
    lv_obj_t *d_warn_icon_ = nullptr;         // 预警图标（随反色切换黑白）
    lv_obj_t *d_warn_label_ = nullptr;        // 预警文案

    // ===== 独立倒计时页状态（countdown_ui.cc）=====
    struct CountdownItem {
        uint32_t remaining_ms = 0;
    };
    static constexpr int kMaxCountdowns = 4;
    CountdownItem countdowns_[kMaxCountdowns] = {};
    int countdown_count_ = 0;                       // 受 DisplayLock 保护
    std::atomic<bool> countdown_active_{false};     // 供其它任务无锁读取
    std::atomic<uint32_t> countdown_alert_clear_ms_{0}; // 到点提醒自动清除时间
    bool countdown_paused_ = false;
    uint32_t countdown_last_tick_ms_ = 0;
    int countdown_last_shown_sec_ = -1;
    // 天气查询页：播报结束（Speaking→Idle）或兜底超时后自动回主页
    uint32_t detail_return_deadline_ms_ = 0;
    bool detail_wait_speaking_ = false;
    bool detail_saw_speaking_ = false;

    // 图片图标（不能用基类的 label，因为我们用 lv_image 而不是 Font Awesome 文字）
    lv_obj_t *wifi_icon_img_ = nullptr;
    lv_obj_t *battery_icon_img_ = nullptr;
    lv_obj_t *battery_pct_label_ = nullptr;  // 电池百分比文字
    lv_obj_t *music_wifi_icon_img_ = nullptr;     // 音乐页面状态栏 WiFi 图标
    lv_obj_t *music_battery_icon_img_ = nullptr;  // 音乐页面状态栏电池图标
    lv_obj_t *music_battery_pct_label_ = nullptr; // 音乐页面状态栏电量文字

    // 数据更新任务句柄
    TaskHandle_t update_task_handle_ = nullptr;
    
    // 系统信息滚动标志（为 true 时暂停 DataUpdateTask 更新，避免锁竞争）
    std::atomic<bool> showing_system_info_{false};
    uint32_t showing_system_info_since_ms_ = 0;   // 系统信息开始滚动的时间（用于超时自动恢复）
    
    // 省电模式：5 分钟无活动后降低刷新频率（1秒 → 5秒）
    std::atomic<bool> power_saving_{false};     // 是否处于省电模式
    uint32_t last_activity_ms_ = 0;             // 上次用户活动的时间（tick 毫秒）
    static const uint32_t IDLE_TIMEOUT_MS = 5 * 60 * 1000;  // 5 分钟无活动进入省电
    static const int NORMAL_REFRESH_MS = 1000;  // 正常刷新间隔 1 秒
    static const int SAVING_REFRESH_MS = 5000;  // 省电刷新间隔 5 秒
    
    // 上次更新的值（用于避免不必要的 UI 刷新）
    int last_min_ = -1;
    time_t last_valid_epoch_ = 0;  // NTP 同步后记录正确的 epoch，用于检测时间被外部篡改
    float last_temp_ = -99.0f;
    float last_humi_ = -99.0f;

    // LVGL flush 回调（将 RGB565 转换为 1-bit 并刷新到 RLCD）
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // UI 创建（实现在 weather_ui.cc / music_ui.cc）
    void SetupWeatherUI();
    void SetupMusicUI();
    void SetupWeatherDetailUI();
    void SetupCountdownUI();      // 实现在 countdown_ui.cc
    void ApplyDisplayMode();
    void RenderCountdownLocked(); // 调用方须持有 DisplayLock
    int PollTransientUi(uint32_t now_ms); // 倒计时递减 + 天气查询页自动返回；返回本次到点个数
    
    // 备忘录
    void LoadMemoFromNvs();   // 从 NVS 加载备忘录到 UI
    
    // 数据更新任务（实现在 data_update_task.cc）
    static void DataUpdateTask(void *arg);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, spi_display_config_t spiconfig, spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();

    // 获取 RLCD 驱动（供外部调用硬件方法，如对比度调节）
    RlcdDriver* rlcd() const { return rlcd_; }

    // 把当前显存（1-bit）导出成 1bpp BMP（62 字节头 + 行对齐位图），用于远程截图
    // out 容量需 >= 62 + ((w+31)/32*4) * h；成功返回 true 并写 *out_len
    bool CaptureScreenBmp(uint8_t *out, size_t cap, size_t *out_len);

    // 软件反色：true=深底白字，false=浅底黑字（面板极性恒 0x20，送显位流取反）。
    // 持锁切换并强制整帧重送一次，切换立即生效，且与 LVGL flush 互斥。
    void SetInversion(bool enabled);
    bool inversion() const;

    // VCOM 电压/对比度 (0x00-0x1F)：持锁设置并强制整帧重送，用于运行时诊断闪烁。
    void SetVcom(uint8_t level);
    uint8_t vcomLevel() const;

    // 1-bit 二值化抖动模式：0=硬阈值(旧行为) 1=Bayer4x4 2=Bayer8x8(默认)
    void SetDitherMode(int mode);
    int GetDitherMode() const;
    void SetThreshold(int thr);      // 模式 0 的二值化阈值
    int GetThreshold() const;
    void RedrawAll();   // 请求整屏重绘（切换抖动模式后调用）
    
    // 获取 AI 对话标签（供 CustomBoard 设置滚动模式显示系统信息等）
    lv_obj_t* GetChatStatusLabel() const { return chat_status_label_; }
    
    // 系统信息滚动控制（供 CustomBoard 设置标志，避免 DataUpdateTask 锁竞争）
    void SetShowingSystemInfo(bool showing) {
        showing_system_info_ = showing;
        showing_system_info_since_ms_ = showing ? lv_tick_get() : 0;
    }
    // 系统信息滚动超过 15 秒自动复位（防止 SetShowingSystemInfo(true) 后 UI 永久停更）
    bool PollSystemInfoTimeout();
    
    // 省电模式：记录用户活动，唤醒省电模式
    void NotifyUserActivity();
    bool IsPowerSaving() const { return power_saving_; }
    
    // 重写小智的 AI 显示方法，适配到左下角卡片
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual bool SetEmotion(const char* emotion) override;
    virtual void ClearChatMessages() override;
    
    // 重写状态栏更新（我们用图片图标，不用 Font Awesome 文字）
    virtual void UpdateStatusBar(bool update_all = false) override;
    
    // 重写主题切换（RLCD 单色屏不需要主题切换，避免基类操作不存在的控件导致崩溃）
    virtual void SetTheme(Theme* theme) override;
    virtual void SetMusicInfo(const char* title, const char* artist) override;
    virtual void SetMusicLyric(const char* lyric) override;
    virtual void SetMusicProgress(uint32_t current_ms, uint32_t total_ms) override;
    virtual void SwitchToMusicPage() override;
    virtual void SwitchToWeatherPage() override;
    virtual void SwitchToHomePage() override;
    
    // 启动数据更新任务（需要在网络连接后调用）
    void StartDataUpdateTask();
    
    // 刷新右下角备忘录列表显示（从 NVS 读取后格式化显示）
    void RefreshMemoDisplay();           // 自动获取锁（外部调用用这个）
    void RefreshMemoDisplayInternal();   // 不获取锁（已持锁时用这个，避免死锁）
    void ApplyDefaultEmotion();          // 应用默认「待命」表情图（不获取锁）
    void CycleDisplayMode();

    // 调试用：返回关键控件状态文本（供 HTTP /dbg 端点排查显示问题）
    std::string GetUiDebugInfo();
    bool IsMusicMode() const { return display_mode_ == MODE_MUSIC; }
    bool IsDetailMode() const { return display_mode_ == MODE_DETAIL; }
    void SwitchToWeatherDetailPage();
    // 刷新天气详情页（调用方需已持有 DisplayLockGuard；内部有数据变更检测）
    void RefreshWeatherDetail();

    // ===== 倒计时（语音 MCP 工具调用）=====
    bool StartCountdown(int seconds);        // 新建倒计时并自动切到倒计时页
    bool PauseCountdown();                  // 暂停所有倒计时（"暂停闹钟"）
    bool ResumeCountdown();                 // 继续倒计时
    bool CancelCountdown();                 // 取消全部倒计时（"取消闹钟"）并返回主页/音乐页
    int GetCountdownRemainingSec() const;   // 当前显示项剩余秒数，无则 0
    bool IsCountdownMode() const { return display_mode_ == MODE_COUNTDOWN; }
    virtual bool IsCountdownActive() const override { return countdown_active_.load(); }
    void MarkCountdownAlert(uint32_t now_ms);            // 记录到点提醒，4 秒后自动清除
    bool ShouldDismissCountdownAlert(uint32_t now_ms);   // 由数据任务在锁外调用，返回 true 时执行 DismissAlert

    // ===== 天气查询：切详情页并在播报结束后自动返回 =====
    void ShowWeatherDetailForQuery(uint32_t auto_return_ms);
};

#endif