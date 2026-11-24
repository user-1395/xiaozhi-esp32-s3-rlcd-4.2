// CustomLcdDisplay 核心类
//
// 负责：
// - 构造/析构（初始化 RLCD 驱动 + LVGL + 创建 UI）
// - LVGL flush 回调（RGB565 → 1-bit 转换）
// - AI 消息适配（重写小智的 SetChatMessage / SetEmotion / ClearChatMessages）
// - 备忘录功能（加载/刷新备忘录列表）
// - 基类方法重写（UpdateStatusBar / SetTheme）
//
// 其他功能拆分到独立文件：
//   rlcd_driver.cc        - RLCD 硬件驱动
//   weather_ui.cc          - 天气站 UI 布局
//   music_ui.cc            - 音乐页 UI 布局
//   data_update_task.cc    - 后台数据更新任务

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_timer.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "lvgl_theme.h"
#include "managers/power_manager.h"
#include "material_symbols.h"
#include "ui_assets.h"
#include "managers/todo_mcp_client.h"

static const char *TAG = "CustomDisplay";

// ===== 1-bit 二值化：有序抖动（Bayer）=====
//
// RLCD 面板只有黑白两态，16bpp 画面必须量化成 1bit。硬阈值会把所有抗锯齿边缘
// （4bpp 中文字形、圆角、圆弧、圆形图标）切成硬阶梯，就是肉眼看到的「锯齿」。
// 有序抖动用像素坐标做稳定阈值，把中间调变成疏密不同的黑点：远看是灰阶、
// 边缘过渡连续；纯白/纯黑仍是纯色；图案只与坐标有关，静态区域不闪。
//   模式 0 = 硬阈值（默认；实测文字最清晰，配合 thr=112 笔画最实）
//   模式 1 = Bayer 4x4（点更细）
//   模式 2 = Bayer 8x8（过渡最平滑）
// 实测结论：本 UI 以汉字/实线为主，抖动会把笔画点碎，故默认关闭抖动，
// 仅把硬阈值定在 112（96..144 扫描后笔画断裂最少的一档）。

// 0..63 的 Bayer 矩阵（先乘 4 再加 2 得 2..254 的阈值）
static const uint8_t kBayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};
static const uint8_t kBayer4[4][4] = {
    { 0, 32,  8, 40},
    {48, 16, 56, 24},
    {12, 44,  4, 36},
    {60, 28, 52, 20},
};

// RGB565 -> 8bit BT.601 亮度（原实现直接拿 RGB565 数值比大小，彩色元素会误判）
static inline uint8_t rgb565_luma(uint16_t v)
{
    const int r5 = (v >> 11) & 0x1F;
    const int g6 = (v >> 5) & 0x3F;
    const int b5 = v & 0x1F;
    const int r8 = (r5 << 3) | (r5 >> 2);
    const int g8 = (g6 << 2) | (g6 >> 4);
    const int b8 = (b5 << 3) | (b5 >> 2);
    return (uint8_t)((r8 * 77 + g8 * 150 + b8 * 29) >> 8);
}

// 取该像素的抖动阈值（2..254）
static inline int dither_threshold(int x, int y, int mode)
{
    if (mode == 1) {
        return (int)kBayer4[y & 3][x & 3] * 4 + 2;
    }
    return (int)kBayer8[y & 7][x & 7] * 4 + 2;
}

// ===== LVGL flush 回调 =====

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *self = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    RlcdDriver *rlcd = self->rlcd_;
    uint16_t *buffer = (uint16_t *)color_p;

    const int scr_w = (int)lv_display_get_horizontal_resolution(disp);
    const int scr_h = (int)lv_display_get_vertical_resolution(disp);

    // 1) 裁剪到屏幕范围（LVGL 可能给出越界区域）
    int x1 = area->x1 < 0 ? 0 : area->x1;
    int y1 = area->y1 < 0 ? 0 : area->y1;
    int x2 = area->x2 >= scr_w ? scr_w - 1 : area->x2;
    int y2 = area->y2 >= scr_h ? scr_h - 1 : area->y2;

    if (x1 <= x2 && y1 <= y2) {
        const int src_w = area->x2 - area->x1 + 1;
        int changed_min_x = scr_w;
        int changed_max_x = -1;
        // 2) 只把该区域写进 1-bit 影子缓冲，同时记录真正发生变化的 x 范围
        const int dither_mode = self->dither_mode_.load();
        const int threshold = self->threshold_.load();
        for (int y = y1; y <= y2; y++) {
            uint16_t *row = buffer + (size_t)(y - area->y1) * src_w + (x1 - area->x1);
            for (int x = x1; x <= x2; x++) {
                const uint8_t luma = rgb565_luma(*row);
                uint8_t color;
                if (dither_mode == 0) {
                    color = (luma >= threshold) ? ColorWhite : ColorBlack;
                } else if (dither_mode == 3 || dither_mode == 4) {
                    // 带状抖动：笔画核心与纯背景保持纯色，只对过渡带做抖动。
                    // 这样圆角/圆弧/斜线得到连续过渡，而汉字笔画不会被点碎。
                    const int lo = (dither_mode == 3) ? 64 : 96;
                    const int hi = (dither_mode == 3) ? 192 : 160;
                    if (luma <= lo) {
                        color = ColorBlack;
                    } else if (luma >= hi) {
                        color = ColorWhite;
                    } else {
                        color = (luma > dither_threshold(x, y, dither_mode)) ? ColorWhite : ColorBlack;
                    }
                } else {
                    color = (luma > dither_threshold(x, y, dither_mode)) ? ColorWhite : ColorBlack;
                }
                if (rlcd->RLCD_SetPixel((uint16_t)x, (uint16_t)y, color)) {
                    if (x < changed_min_x) changed_min_x = x;
                    if (x > changed_max_x) changed_max_x = x;
                }
                row++;
            }
        }
        if (changed_max_x >= changed_min_x) {
            // 3) 面板按 2 像素对寻址，条带边界向外对齐
            int a1 = changed_min_x & ~1;
            int a2 = changed_max_x | 1;
            if (a2 > scr_w - 1) a2 = scr_w - 1;
            self->FlushAddRange(a1, a2);
        }
    }

    // 4) 帧末统一提交（整帧或逐条带局部刷新）
    if (lv_display_flush_is_last(disp)) {
        self->FlushCommit();
    }
    lv_display_flush_ready(disp);
}

// 切换 1-bit 抖动模式（0=硬阈值 1=Bayer4 2=Bayer8），供 /dither 端点在线 A/B 对比
void CustomLcdDisplay::SetDitherMode(int mode)
{
    if (mode < 0 || mode > 4) {
        return;
    }
    dither_mode_.store(mode);
    ESP_LOGI(TAG, "1-bit 抖动模式切换为 %d (0=阈值 1=Bayer4 2=Bayer8 3/4=带状抖动)", mode);
}

void CustomLcdDisplay::SetThreshold(int thr)
{
    if (thr < 16 || thr > 240) {
        return;
    }
    threshold_.store(thr);
    ESP_LOGI(TAG, "1-bit 阈值切换为 %d", thr);
}

int CustomLcdDisplay::GetThreshold() const
{
    return threshold_.load();
}

int CustomLcdDisplay::GetDitherMode() const
{
    return dither_mode_.load();
}

// 请求整屏重绘（切换抖动模式后让 LVGL 重新光栅化，截图才会反映新模式）
void CustomLcdDisplay::RedrawAll()
{
    DisplayLockGuard lock(this);
    lv_obj_invalidate(lv_screen_active());
}

// 软件反色切换：设标志 + 强制整帧重送。
// 反色只改送显位流、不改 DispBuffer，故 invalidate 不会触发重绘（内容未变会被 skip），
// 必须在这里直接整帧重送一次才能立即生效；持锁避免与 LVGL flush 任务竞争 SPI/显存。
void CustomLcdDisplay::SetInversion(bool enabled)
{
    if (rlcd_ == nullptr) return;
    DisplayLockGuard lock(this);
    rlcd_->SetInversion(enabled);
    PowerManager::ApbMaxGuard apb_guard;
    PowerManager::NoLightSleepGuard sleep_guard;
    rlcd_->RLCD_Display();
}

bool CustomLcdDisplay::inversion() const
{
    return rlcd_ != nullptr && rlcd_->inversion();
}

// VCOM 电压设置：改电压后需重送一帧才能在面板上体现新对比度。
void CustomLcdDisplay::SetVcom(uint8_t level)
{
    if (rlcd_ == nullptr) return;
    DisplayLockGuard lock(this);
    rlcd_->SetContrast(level);
    PowerManager::ApbMaxGuard apb_guard;
    PowerManager::NoLightSleepGuard sleep_guard;
    rlcd_->RLCD_Display();
}

uint8_t CustomLcdDisplay::vcomLevel() const
{
    return rlcd_ != nullptr ? rlcd_->vcom_level() : 0x11;
}

// ===== v12: 持续刷新任务（对齐微雪官方 U8G2 例程快速刷新）=====
// 官方深色背景不闪：每 13ms 重送整帧，深色像素电荷持续维持。
// 小智原本事件驱动静态刷新（画面不变跳过 SPI），深色像素电荷泄漏→左上角 band 边界闪。
// 本任务周期性重送整帧。DisplayLockGuard=lvgl_port_lock，与 LVGL flush 任务互斥 SPI/显存。
void CustomLcdDisplay::RefreshTask(void *arg)
{
    CustomLcdDisplay *self = (CustomLcdDisplay *)arg;
    ESP_LOGI(TAG, "持续刷新任务启动，默认间隔 %d ms", self->refresh_interval_ms_.load());
    while (self->refresh_running_.load()) {
        const int interval = self->refresh_interval_ms_.load();
        vTaskDelay(pdMS_TO_TICKS(interval));
        if (!self->refresh_running_.load() || self->rlcd_ == nullptr) {
            continue;
        }
        DisplayLockGuard lock(self);
        if (!lock) {
            continue;
        }
        PowerManager::ApbMaxGuard apb_guard;
        PowerManager::NoLightSleepGuard sleep_guard;
        self->rlcd_->RLCD_Display();   // 即使画面不变也整帧重送，维持像素电荷
        self->stat_full_++;
        self->stat_bytes_ += (uint64_t)self->rlcd_->display_len();
    }
    self->refresh_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void CustomLcdDisplay::StartRefreshTask()
{
    if (refresh_task_handle_ != nullptr) {
        return;   // 已启动
    }
    refresh_running_.store(true);
    // 低优先级(1) + 4KB 栈：只做加锁+整帧 SPI 重送，不碰 UI/网络
    xTaskCreate(RefreshTask, "rlcd_auto_refresh", 4096, this, 1, &refresh_task_handle_);
    ESP_LOGI(TAG, "持续刷新已启用：每 %d ms 整帧重送（深色防闪）", refresh_interval_ms_.load());
}

void CustomLcdDisplay::SetRefresh(bool enabled, int interval_ms)
{
    if (interval_ms >= 1 && interval_ms <= 200) {
        refresh_interval_ms_.store(interval_ms);
        ESP_LOGI(TAG, "持续刷新间隔调整为 %d ms", interval_ms);
    }
    if (enabled) {
        StartRefreshTask();
    } else {
        refresh_running_.store(false);
        ESP_LOGI(TAG, "持续刷新已关闭（恢复静态刷新，画面不变跳过 SPI）");
    }
}

// 累积/合并脏区条带：间隙小于 kFlushMergeGap 的条带合并，条带过多则退化为整帧
void CustomLcdDisplay::FlushAddRange(int x1, int x2)
{
    if (x1 > x2) {
        return;
    }
    for (int i = 0; i < flush_range_count_; i++) {
        if (x1 <= flush_ranges_[i].x2 + kFlushMergeGap &&
            x2 + kFlushMergeGap >= flush_ranges_[i].x1) {
            if (x1 < flush_ranges_[i].x1) flush_ranges_[i].x1 = x1;
            if (x2 > flush_ranges_[i].x2) flush_ranges_[i].x2 = x2;
            // 合并后可能与后面的条带再次相邻/重叠，反复归并直到稳定
            bool merged = true;
            while (merged) {
                merged = false;
                for (int a = 0; a < flush_range_count_ && !merged; a++) {
                    for (int b = a + 1; b < flush_range_count_; b++) {
                        if (flush_ranges_[a].x1 <= flush_ranges_[b].x2 + kFlushMergeGap &&
                            flush_ranges_[b].x1 <= flush_ranges_[a].x2 + kFlushMergeGap) {
                            if (flush_ranges_[b].x1 < flush_ranges_[a].x1) flush_ranges_[a].x1 = flush_ranges_[b].x1;
                            if (flush_ranges_[b].x2 > flush_ranges_[a].x2) flush_ranges_[a].x2 = flush_ranges_[b].x2;
                            for (int k = b; k < flush_range_count_ - 1; k++) {
                                flush_ranges_[k] = flush_ranges_[k + 1];
                            }
                            flush_range_count_--;
                            merged = true;
                            break;
                        }
                    }
                }
            }
            return;
        }
    }
    if (flush_range_count_ >= kMaxFlushRanges) {
        flush_force_full_ = true;
        return;
    }
    flush_ranges_[flush_range_count_].x1 = x1;
    flush_ranges_[flush_range_count_].x2 = x2;
    flush_range_count_++;
}

// 帧末提交：首帧/条带过碎/覆盖过宽 → 整帧；否则逐条带只发变化的部分
void CustomLcdDisplay::FlushCommit()
{
    stat_frames_++;

    const int scr_w = (int)lv_display_get_horizontal_resolution(display_);
    const bool nothing_changed = (flush_range_count_ == 0 && !flush_force_full_);

    if (nothing_changed && !flush_first_frame_) {
        stat_skip_++;   // 本帧所有像素与显存一致，完全跳过 SPI
    } else {
        int covered = 0;
        for (int i = 0; i < flush_range_count_; i++) {
            covered += flush_ranges_[i].x2 - flush_ranges_[i].x1 + 1;
        }
        // 覆盖超过 60% 宽度时，逐条带的命令开销大于省下的数据量，直接整帧
        // v8 已验证：强制整帧后左上角闪烁依旧，故排除局部刷新，恢复原逻辑。
        const bool use_full = flush_first_frame_ || flush_force_full_ ||
                              (covered * 5 >= scr_w * 3);
        const int64_t t0 = esp_timer_get_time();
        // SPI 传输期间需要 APB 80 MHz 且不能进入 light sleep
        PowerManager::ApbMaxGuard apb_guard;
        PowerManager::NoLightSleepGuard sleep_guard;
        if (use_full) {
            rlcd_->RLCD_Display();
            stat_full_++;
            stat_bytes_ += (uint64_t)rlcd_->display_len();
        } else {
            for (int i = 0; i < flush_range_count_; i++) {
                rlcd_->RLCD_DisplayXRange((uint16_t)flush_ranges_[i].x1,
                                          (uint16_t)flush_ranges_[i].x2);
            }
            stat_partial_++;
            const int rows_per_pair = rlcd_->height() >> 2;
            for (int i = 0; i < flush_range_count_; i++) {
                const int pairs = (flush_ranges_[i].x2 >> 1) - (flush_ranges_[i].x1 >> 1) + 1;
                stat_bytes_ += (uint64_t)pairs * rows_per_pair;
            }
        }
        stat_transfer_us_ += (uint64_t)(esp_timer_get_time() - t0);
        flush_first_frame_ = false;
    }

    flush_range_count_ = 0;
    flush_force_full_ = false;

    // 每 120 帧输出一次统计，便于实机对比局部刷新前后
    if ((stat_frames_ % 120) == 0) {
        ESP_LOGI(TAG,
                 "刷新统计: 帧=%u 整帧=%u 局部=%u 跳过=%u 平均字节/帧=%.0f 平均耗时/帧=%.0f us",
                 (unsigned)stat_frames_, (unsigned)stat_full_, (unsigned)stat_partial_,
                 (unsigned)stat_skip_,
                 (double)stat_bytes_ / (double)stat_frames_,
                 (double)stat_transfer_us_ / (double)stat_frames_);
    }
}

// ===== 构造 / 析构 =====

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    spi_display_config_t spiconfig,
    spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height)
{
    // 1. 初始化 RLCD 硬件驱动
    rlcd_ = new RlcdDriver(spiconfig, width, height, spi_host);

    // 2. 初始化 LVGL
    ESP_LOGI(TAG, "初始化 LVGL");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.task_stack = 10240;   // 默认 7168 在 64px 时钟 + 详情页重绘时曾栈溢出
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    int transfer = width * height;
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 3. 初始化 RLCD 屏幕
    ESP_LOGI(TAG, "初始化 RLCD 屏幕");
    // P2 #19：显存/LUT 分配失败时 RLCD_Init 返回 false，这里只告警降级，绝不 panic
    if (rlcd_ == nullptr || !rlcd_->RLCD_Init()) {
        ESP_LOGE(TAG, "RLCD 初始化失败，显示功能降级（不 panic），继续创建 UI");
    }

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }

    // 4. 创建天气页 + 音乐页 UI
    ESP_LOGI(TAG, "创建天气页 + 音乐页 UI");
    SetupWeatherUI();
    SetupMusicUI();
    SetupWeatherDetailUI();
    SetupCountdownUI();
    // 告诉显示框架：当前自定义 UI 已经初始化完成
    // 否则基类的 SetStatus/ShowNotification 会一直误判为“UI 未准备好”
    setup_ui_called_ = true;
    ApplyDisplayMode();

    // 5. 启动时从 NVS 加载上次保存的备忘录
    LoadMemoFromNvs();
}

CustomLcdDisplay::~CustomLcdDisplay() {
    refresh_running_.store(false);
    if (refresh_task_handle_) {
        vTaskDelete(refresh_task_handle_);
    }
    if (update_task_handle_) {
        vTaskDelete(update_task_handle_);
    }
    delete rlcd_;
}

// ===== 备忘录功能 =====

void CustomLcdDisplay::LoadMemoFromNvs() {
    // 直接调用 RefreshMemoDisplay 从 NVS 读取并更新 UI
    RefreshMemoDisplay();
}

// 内部版本：不获取锁（调用者必须已持有 DisplayLock）
// 每行 = ★ 图标 + 文本（文本自动换行），和参考界面的待办列表一致
void CustomLcdDisplay::RefreshMemoDisplayInternal() {
    if (!memo_rows_) return;

    // 清空旧行（lv_obj_clean 会一并删除子对象）
    lv_obj_clean(memo_rows_);

    // 从 NVS 读取 JSON 数组
    Settings settings("memo", false);
    std::string json_str = settings.GetString("items", "");

    cJSON *arr = json_str.empty() ? nullptr : cJSON_Parse(json_str.c_str());
    const int count = (arr && cJSON_IsArray(arr)) ? cJSON_GetArraySize(arr) : 0;

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(memo_rows_);
        lv_obj_set_style_text_font(empty, &font_puhui_16_1, 0);
        lv_obj_set_style_text_color(empty, lv_color_black(), 0);
        lv_label_set_text(empty, "暂无待办");
        if (arr) cJSON_Delete(arr);
        return;
    }

    const int row_w = memo_rows_w_;
    // P2 #26：与抓取侧共享上限；显示侧受卡片高度限制最多 kTodoDisplayMaxItems(6) 条，
    // 第 7~10 条为已知的上屏裁剪（抓取侧仍缓冲 10 条），非静默丢数据。
    const int kMaxItems = kTodoDisplayMaxItems;

    lv_obj_t *boxes[kMaxItems] = {nullptr};
    lv_obj_t *dashes[kMaxItems] = {nullptr};
    int built = 0;

    for (int i = 0; i < count && i < kMaxItems; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *t = cJSON_GetObjectItem(item, "t");
        cJSON *c = cJSON_GetObjectItem(item, "c");

        const char *time_str =
            (t != nullptr && cJSON_IsString(t) && t->valuestring != nullptr) ? t->valuestring : "";
        const char *body_str =
            (c != nullptr && cJSON_IsString(c) && c->valuestring != nullptr) ? c->valuestring : "";

        // 项容器：竖排两行（第一行 ★+时间，第二行 事项），末尾附一条虚线
        lv_obj_t *box = lv_obj_create(memo_rows_);
        lv_obj_set_width(box, row_w);
        lv_obj_set_height(box, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_style_pad_row(box, 0, 0);   // 行内两行紧贴，保证 3 条待办能同时放进 199px
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        // 第一行：★ + 时间
        lv_obj_t *head = lv_obj_create(box);
        lv_obj_set_width(head, row_w);
        lv_obj_set_height(head, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(head, 0, 0);
        lv_obj_set_style_pad_all(head, 0, 0);
        lv_obj_set_style_pad_column(head, 4, 0);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *icon = lv_label_create(head);
        lv_obj_set_style_text_font(icon, &font_material_symbols_16_1, 0);
        lv_obj_set_style_text_color(icon, lv_color_black(), 0);
        lv_label_set_text(icon, MATERIAL_SYMBOLS_STAR);

        bool ascii_time = true;
        for (const char *p = time_str; *p != 0; ++p) {
            if (static_cast<unsigned char>(*p) >= 0x80) { ascii_time = false; break; }
        }
        lv_obj_t *time_lbl = lv_label_create(head);
        lv_obj_set_style_text_font(time_lbl, ascii_time ? &font_puhui_16_1 : &font_puhui_16_1_full, 0);
        lv_obj_set_style_text_color(time_lbl, lv_color_black(), 0);
        lv_label_set_text(time_lbl, (time_str[0] != 0) ? time_str : "--:--");

        // 第二行：事项文本（占满整行宽，自动换行）
        lv_obj_t *text = lv_label_create(box);
        lv_obj_set_style_text_font(text, &font_puhui_16_1_full, 0);
        lv_obj_set_style_text_color(text, lv_color_black(), 0);
        lv_obj_set_style_text_line_space(text, 1, 0);
        lv_obj_set_width(text, row_w);
        lv_obj_set_style_max_height(text, 39, 0);   // 两行 19+1+19=39，最多两行，超出用省略号
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_label_set_text(text, (body_str[0] != 0) ? body_str : "备忘提醒");

        boxes[built] = box;

        // 项与项之间的虚线（最后一项之后不加）
        if (i + 1 < count && i + 1 < kMaxItems) {
            lv_obj_t *ln = lv_line_create(box);
            memo_dash_pts_[built][0].x = 0;
            memo_dash_pts_[built][0].y = 0;
            memo_dash_pts_[built][1].x = row_w - 1;
            memo_dash_pts_[built][1].y = 0;
            lv_line_set_points(ln, memo_dash_pts_[built], 2);
            lv_obj_set_size(ln, row_w, 3);
            lv_obj_set_style_line_width(ln, 2, 0);
            lv_obj_set_style_line_color(ln, lv_color_black(), 0);
            lv_obj_set_style_line_rounded(ln, false, 0);
            lv_obj_set_style_line_dash_width(ln, 5, 0);
            lv_obj_set_style_line_dash_gap(ln, 4, 0);
            dashes[built] = ln;
        }
        built++;
    }

    // 按容器实际高度裁剪：放不下的项丢掉，改成「…还有N条」
    lv_obj_update_layout(memo_rows_);
    const int avail_h = lv_obj_get_height(memo_rows_);
    int used_h = 0;
    int keep = 0;
    for (int i = 0; i < built; i++) {
        const int h = lv_obj_get_height(boxes[i]);
        if (used_h + h > avail_h) break;
        used_h += h + 3;   // 3 = memo_rows_ 的 pad_row
        keep++;
    }
    for (int i = keep; i < built; i++) {
        lv_obj_delete(boxes[i]);
        boxes[i] = nullptr;
        dashes[i] = nullptr;
    }
    // 裁剪后最后一项不该再带虚线
    if (keep > 0 && keep < built && dashes[keep - 1] != nullptr) {
        lv_obj_delete(dashes[keep - 1]);
        dashes[keep - 1] = nullptr;
    }

    {
        char dbg[160];
        int n = std::snprintf(dbg, sizeof(dbg), "待办布局: 可用 %d, built %d, keep %d, 高", avail_h, built, keep);
        for (int i = 0; i < keep && n > 0 && n < (int)sizeof(dbg); i++) {
            n += std::snprintf(dbg + n, sizeof(dbg) - n, " %d", (int)lv_obj_get_height(boxes[i]));
        }
        ESP_LOGI(TAG, "%s", dbg);
    }

    // 不再显示「…还有N条」：用户按时间线依次完成，未显示的下一条会依次顶上

    cJSON_Delete(arr);
    ESP_LOGI(TAG, "待办列表已刷新，共 %d 条", count);
}

// 外部版本：自动获取锁（供 MCP 工具等外部调用）
void CustomLcdDisplay::RefreshMemoDisplay() {
    DisplayLockGuard lock(this);
    RefreshMemoDisplayInternal();
}

// ===== AI 消息适配（重写小智的方法，只更新左下角卡片）=====

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!content || strlen(content) == 0) return;
    if (chat_status_label_ == nullptr) {
        // 天气页标签不存在时至少同步音乐页文案，避免空指针写入
        if (music_chat_status_label_) lv_label_set_text(music_chat_status_label_, content);
        return;
    }

    // 停止可能正在运行的滚动动画（系统信息或之前的 AI 滚动）
    lv_anim_delete(chat_status_label_, nullptr);
    
    // 停止系统信息滚动，恢复 DataUpdateTask 更新
    SetShowingSystemInfo(false);
    
    // 设置文本内容
    lv_label_set_text(chat_status_label_, content);
    lv_label_set_long_mode(chat_status_label_, LV_LABEL_LONG_WRAP);
    
    // 先恢复居中对齐（正常模式），计算内容高度
    lv_obj_align(chat_status_label_, LV_ALIGN_LEFT_MID, kAiChatTextOffsetX, 0);
    
    // 检查内容是否超出父容器（chat_inner）的可见高度
    lv_obj_update_layout(chat_status_label_);
    int label_h = lv_obj_get_height(chat_status_label_);
    // 从父容器动态获取高度，不硬编码（父容器是 chat_inner）
    lv_obj_t *parent = lv_obj_get_parent(chat_status_label_);
    int visible_h = parent ? lv_obj_get_content_height(parent) : 108;
    
    if (label_h > visible_h) {
        // 超长内容：切换到 TOP_LEFT 绝对定位后启用滚动
        // （和鱼咬尾同理，LEFT_MID 对齐会干扰动画的 set_y）
        const int text_x = kAiChatTextOffsetX;
        lv_obj_align(chat_status_label_, LV_ALIGN_TOP_LEFT, text_x, 0);
        
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, chat_status_label_);
        lv_anim_set_values(&a, 0, -(label_h - visible_h));  // 从顶部滚到底部
        lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒
        lv_anim_set_duration(&a, (label_h - visible_h) * 50);  // 速度：每像素 50ms
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 2000);  // 滚完后暂停 2 秒再重新开始
        lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
            lv_obj_set_y((lv_obj_t *)obj, v);
        });
        lv_anim_start(&a);
        
        ESP_LOGI("CustomLcdDisplay", "AI 回答过长（%dpx > %dpx），启用慢速滚动", label_h, visible_h);
    }

    // 音乐页同步显示 AI 文案
    if (music_chat_status_label_) {
        lv_label_set_long_mode(music_chat_status_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(music_chat_status_label_, content);
    }
}

bool CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    
    // 1. 更新左侧文字（完整映射小智所有 21 种表情 + 额外状态）
    const char* text = "待命";
    if (strcmp(emotion, "neutral") == 0)         text = "待命";
    else if (strcmp(emotion, "happy") == 0)      text = "开心";
    else if (strcmp(emotion, "laughing") == 0)   text = "大笑";
    else if (strcmp(emotion, "funny") == 0)      text = "搞笑";
    else if (strcmp(emotion, "sad") == 0)        text = "难过";
    else if (strcmp(emotion, "angry") == 0)      text = "生气";
    else if (strcmp(emotion, "crying") == 0)     text = "哭泣";
    else if (strcmp(emotion, "loving") == 0)     text = "喜爱";
    else if (strcmp(emotion, "embarrassed") == 0) text = "害羞";
    else if (strcmp(emotion, "surprised") == 0)  text = "惊讶";
    else if (strcmp(emotion, "shocked") == 0)    text = "震惊";
    else if (strcmp(emotion, "thinking") == 0)   text = "思考";
    else if (strcmp(emotion, "winking") == 0)    text = "眨眼";
    else if (strcmp(emotion, "cool") == 0)       text = "耍酷";
    else if (strcmp(emotion, "relaxed") == 0)    text = "放松";
    else if (strcmp(emotion, "delicious") == 0)  text = "好吃";
    else if (strcmp(emotion, "kissy") == 0)      text = "亲亲";
    else if (strcmp(emotion, "confident") == 0)  text = "自信";
    else if (strcmp(emotion, "sleepy") == 0)     text = "犯困";
    else if (strcmp(emotion, "silly") == 0)      text = "调皮";
    else if (strcmp(emotion, "confused") == 0)   text = "困惑";
    // 额外状态
    else if (strcmp(emotion, "fear") == 0)       text = "害怕";
    else if (strcmp(emotion, "disgusted") == 0)  text = "嫌弃";
    else if (strcmp(emotion, "microchip_ai") == 0) text = "就绪";
    // 未知情绪也显示中文，不显示英文原文
    else                                         text = "待命";
    
    if (emotion_label_) {
        lv_label_set_text(emotion_label_, text);
    }
    if (music_emotion_label_) {
        lv_label_set_text(music_emotion_label_, text);
    }
    
    // 2. 尝试加载小智自带的 emoji 图片（天气页 + 音乐页同步更新）
    bool has_image = false;
    if (current_theme_) {
        auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
        auto image = emoji_collection ? emoji_collection->GetEmojiImage(emotion) : nullptr;
        has_image = (image && !image->IsGif());
        
        // 天气页 emoji
        if (emotion_img_) {
            if (has_image) {
                lv_image_set_src(emotion_img_, image->image_dsc());
                lv_obj_remove_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // 音乐页 emoji（同步显示相同的表情图片）
        if (music_emotion_img_) {
            if (has_image) {
                lv_image_set_src(music_emotion_img_, image->image_dsc());
                lv_obj_remove_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    // 资源包未带 emoji 素材时，用内置猫脸线稿兜底常驻显示（天气页 + 音乐页同步）
    lv_obj_t *faces[2] = {ai_face_, music_ai_face_};
    for (lv_obj_t *face : faces) {
        if (!face) continue;
        if (has_image) {
            lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
        }
    }

    return true;
}

// 不获取锁：调用者必须已持有 DisplayLock（供 SetupWeatherUI 使用）
void CustomLcdDisplay::ApplyDefaultEmotion() {
    auto emoji_collection = current_theme_
                                ? static_cast<LvglTheme*>(current_theme_)->emoji_collection()
                                : nullptr;
    auto image = emoji_collection ? emoji_collection->GetEmojiImage("neutral") : nullptr;
    const bool has_image = (image && !image->IsGif());
    if (has_image && emotion_img_) {
        lv_image_set_src(emotion_img_, image->image_dsc());
        lv_obj_remove_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
    } else if (emotion_img_) {
        lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
    }
    // 无素材图片 → 显示内置猫脸（天气页 + 音乐页）
    lv_obj_t *faces[2] = {ai_face_, music_ai_face_};
    for (lv_obj_t *face : faces) {
        if (!face) continue;
        if (has_image) {
            lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (emotion_label_) lv_label_set_text(emotion_label_, "待命");
}

void CustomLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // 空闲态保留「AI 待命」占位文案（与参考界面一致），不要清成空白
    if (chat_status_label_) lv_label_set_text(chat_status_label_, "AI 待命");
    if (music_chat_status_label_) lv_label_set_text(music_chat_status_label_, "");
    // 表情不清除，保持常驻
}

bool CustomLcdDisplay::PollSystemInfoTimeout() {
    if (!showing_system_info_) return false;
    if (showing_system_info_since_ms_ == 0) return false;
    if (lv_tick_elaps(showing_system_info_since_ms_) < 15000) return false;
    showing_system_info_ = false;
    showing_system_info_since_ms_ = 0;
    return true;
}

// ===== 重写状态栏更新（禁用基类的 Font Awesome 文字更新）=====

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    // 不调用基类实现！
    // 基类会尝试用 lv_label_set_text 更新 network_label_ 和 battery_label_，
    // 但那些是隐藏的占位标签。我们自己的图片图标由 DataUpdateTask 管理。
}

// ===== 重写主题切换 =====

void CustomLcdDisplay::SetTheme(Theme* theme) {
    // RLCD 是 1-bit 单色屏，只有黑白两色，不需要主题切换。
    // 基类的 SetTheme 会操作 container_、content_、top_bar_ 等控件，
    // 我们的天气站 UI 没有创建这些，直接跳过避免崩溃。
    
    // 但需要保存 theme 指针，SetEmotion 需要用它来加载 emoji 图片
    current_theme_ = theme;
    ESP_LOGI(TAG, "RLCD 单色屏，跳过主题切换（已保存 theme 指针）");
}

void CustomLcdDisplay::ApplyDisplayMode() {
    // 先隐藏所有页面
    if (weather_page_) lv_obj_add_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
    if (music_page_) lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
    if (detail_page_) lv_obj_add_flag(detail_page_, LV_OBJ_FLAG_HIDDEN);
    if (countdown_page_) lv_obj_add_flag(countdown_page_, LV_OBJ_FLAG_HIDDEN);

    // 显示当前页面
    switch (display_mode_) {
        case MODE_WEATHER:
            if (weather_page_) lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_MUSIC:
            if (music_page_) lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_DETAIL:
            if (detail_page_) lv_obj_remove_flag(detail_page_, LV_OBJ_FLAG_HIDDEN);
            RefreshWeatherDetail();   // 切进来立刻填一次数据，不等下一个刷新周期
            break;
        case MODE_COUNTDOWN:
            if (countdown_page_) lv_obj_remove_flag(countdown_page_, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

void CustomLcdDisplay::CycleDisplayMode() {
    DisplayLockGuard lock(this);
    // 倒计时页/音乐播放中禁止手动切页，保证关键界面停留
    if (countdown_active_.load()) {
        ESP_LOGI(TAG, "倒计时进行中，忽略手动切页");
        return;
    }
    if (Application::GetInstance().IsMusicPlaying()) {
        ESP_LOGI(TAG, "音乐播放中，保持音乐页");
        return;
    }
    detail_return_deadline_ms_ = 0;
    // 三页循环：天气 → 天气详情 → 音乐 → 天气（倒计时页只在收到指令时进入）
    switch (display_mode_) {
        case MODE_WEATHER: display_mode_ = MODE_DETAIL; break;
        case MODE_DETAIL:  display_mode_ = MODE_MUSIC; break;
        case MODE_MUSIC:   display_mode_ = MODE_WEATHER; break;
        case MODE_COUNTDOWN: display_mode_ = MODE_WEATHER; break;
    }
    ApplyDisplayMode();
    const char* name = (display_mode_ == MODE_MUSIC)   ? "音乐页"
                       : (display_mode_ == MODE_DETAIL) ? "天气详情页"
                                                        : "天气页";
    ESP_LOGI(TAG, "页面切换: %s", name);
}

void CustomLcdDisplay::SetMusicInfo(const char* title, const char* artist) {
    DisplayLockGuard lock(this);
    if (music_title_label_ == nullptr || music_artist_label_ == nullptr) {
        return;
    }
    lv_label_set_text(music_title_label_, (title && strlen(title) > 0) ? title : "未知歌曲");
    lv_label_set_text(music_artist_label_, (artist && strlen(artist) > 0) ? artist : "未知歌手");
}

void CustomLcdDisplay::SetMusicLyric(const char* lyric) {
    DisplayLockGuard lock(this);
    if (music_lyric_label_ == nullptr) {
        return;
    }

    // 歌词格式："上一句\n当前句\n下一句"（由 application.cc 拼接）
    // 如果没有 \n 分隔符，说明是单行文本（如错误提示），直接显示在当前行
    std::string text(lyric ? lyric : "");
    std::string prev_line, curr_line, next_line;

    size_t first_nl = text.find('\n');
    if (first_nl != std::string::npos) {
        prev_line = text.substr(0, first_nl);
        size_t second_nl = text.find('\n', first_nl + 1);
        if (second_nl != std::string::npos) {
            curr_line = text.substr(first_nl + 1, second_nl - first_nl - 1);
            next_line = text.substr(second_nl + 1);
        } else {
            curr_line = text.substr(first_nl + 1);
        }
    } else {
        // 单行文本（错误提示等），只显示在当前行
        curr_line = text;
    }

    // 更新三个 label
    if (music_lyric_prev_label_) {
        lv_label_set_text(music_lyric_prev_label_, prev_line.c_str());
    }
    lv_label_set_text(music_lyric_label_, curr_line.c_str());
    if (music_lyric_next_label_) {
        lv_label_set_text(music_lyric_next_label_, next_line.c_str());
    }
}

void CustomLcdDisplay::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    DisplayLockGuard lock(this);
    if (music_progress_bar_ == nullptr || music_progress_label_ == nullptr) {
        return;
    }

    if (total_ms > 0) {
        // 有总时长（来自歌词）：正常显示进度条和 "当前 / 总时长"
        if (current_ms > total_ms) {
            current_ms = total_ms;
        }
        lv_bar_set_range(music_progress_bar_, 0, static_cast<int32_t>(total_ms));
        lv_bar_set_value(music_progress_bar_, static_cast<int32_t>(current_ms), LV_ANIM_OFF);

        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu / %02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60),
                 static_cast<unsigned long>(total_ms / 60000),
                 static_cast<unsigned long>((total_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    } else {
        // 无总时长（没有歌词）：进度条不动，只显示已播放时间
        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%02lu:%02lu",
                 static_cast<unsigned long>(current_ms / 60000),
                 static_cast<unsigned long>((current_ms / 1000) % 60));
        lv_label_set_text(music_progress_label_, progress_text);
    }
}

void CustomLcdDisplay::SwitchToMusicPage() {
    DisplayLockGuard lock(this);
    if (countdown_active_.load()) {
        ESP_LOGI(TAG, "倒计时进行中，不切到音乐页");
        return;
    }
    detail_return_deadline_ms_ = 0;
    if (display_mode_ != MODE_MUSIC) {
        display_mode_ = MODE_MUSIC;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到音乐页");
    }
    // 进入音乐页时立即向飞牛 MCP 拉一次 music_status（下一次轮询会提前唤醒）
    TodoMcpClient::GetInstance().RequestRefresh();
}

void CustomLcdDisplay::SwitchToWeatherPage() {
    DisplayLockGuard lock(this);
    // 倒计时/音乐播放期间不允许离开；音乐自然结束后 music_playing_ 已为 false，
    // 会正常走这里返回主页。
    if (countdown_active_.load()) {
        ESP_LOGI(TAG, "倒计时进行中，保持倒计时页");
        return;
    }
    if (Application::GetInstance().IsMusicPlaying()) {
        ESP_LOGI(TAG, "音乐播放中，保持音乐页");
        return;
    }
    detail_return_deadline_ms_ = 0;
    if (display_mode_ != MODE_WEATHER) {
        display_mode_ = MODE_WEATHER;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到天气页");
    }
}

void CustomLcdDisplay::SwitchToHomePage() {
    DisplayLockGuard lock(this);
    // 音乐停止/播完/失败后的强制返回：不受 IsMusicPlaying 保护条件影响，确保回到天气主页
    if (countdown_active_.load()) {
        ESP_LOGI(TAG, "倒计时进行中，保持倒计时页");
        return;
    }
    detail_return_deadline_ms_ = 0;
    detail_wait_speaking_ = false;
    detail_saw_speaking_ = false;
    if (display_mode_ != MODE_WEATHER) {
        display_mode_ = MODE_WEATHER;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "返回天气主页");
    }
}

void CustomLcdDisplay::SwitchToWeatherDetailPage() {
    DisplayLockGuard lock(this);
    if (countdown_active_.load()) {
        ESP_LOGI(TAG, "倒计时进行中，不切到天气详情页");
        return;
    }
    if (Application::GetInstance().IsMusicPlaying()) {
        ESP_LOGI(TAG, "音乐播放中，保持音乐页");
        return;
    }
    // 手动切详情页不带播报自动返回
    detail_return_deadline_ms_ = 0;
    detail_wait_speaking_ = false;
    detail_saw_speaking_ = false;
    if (display_mode_ != MODE_DETAIL) {
        display_mode_ = MODE_DETAIL;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "切换到天气详情页");
    }
}

// ===== 省电模式 =====

void CustomLcdDisplay::NotifyUserActivity() {
    last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (power_saving_) {
        power_saving_ = false;
        ESP_LOGI(TAG, "用户活动检测到，退出省电模式");
    }
}

// ===== 调试信息（供 HTTP /dbg 端点排查显示问题） =====

std::string CustomLcdDisplay::GetUiDebugInfo() {
    DisplayLockGuard lock(this);
    char buf[1024];
    const char *txt = chat_status_label_ ? lv_label_get_text(chat_status_label_) : "(null)";
    lv_obj_t *parent = chat_status_label_ ? lv_obj_get_parent(chat_status_label_) : nullptr;
    const int pw = parent ? lv_obj_get_width(parent) : -1;
    const int ph = parent ? lv_obj_get_height(parent) : -1;
    const int cw = parent ? lv_obj_get_content_width(parent) : -1;
    const int ch = parent ? lv_obj_get_content_height(parent) : -1;
    const void *img_src = nullptr;
    bool img_hidden = true;
    int iw = 0;
    int ih = 0;
    if (emotion_img_) {
        img_src = (const void *)lv_image_get_src(emotion_img_);
        img_hidden = lv_obj_has_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
        iw = lv_obj_get_width(emotion_img_);
        ih = lv_obj_get_height(emotion_img_);
    }
    const int csx = chat_status_label_ ? lv_obj_get_x(chat_status_label_) : -1;
    const int csy = chat_status_label_ ? lv_obj_get_y(chat_status_label_) : -1;
    const int csw = chat_status_label_ ? lv_obj_get_width(chat_status_label_) : -1;
    const int csh = chat_status_label_ ? lv_obj_get_height(chat_status_label_) : -1;
    const bool cs_hidden = chat_status_label_ &&
                           lv_obj_has_flag(chat_status_label_, LV_OBJ_FLAG_HIDDEN);
    const char *emo_txt = emotion_label_ ? lv_label_get_text(emotion_label_) : "(null)";
    bool has_theme = (current_theme_ != nullptr);
    bool has_collection = false;
    int emoji_count = -1;
    if (current_theme_) {
        auto coll = static_cast<LvglTheme *>(current_theme_)->emoji_collection();
        has_collection = (coll != nullptr);
        if (coll) {
            emoji_count = coll->Count();
        }
    }
    const int n = snprintf(buf, sizeof(buf),
             "chat_status: text=\"%s\" hidden=%d pos=%d,%d size=%dx%d\n"
             "parent: size=%dx%d content=%dx%d\n"
             "emotion_label: text=\"%s\"\n"
             "emotion_img: hidden=%d size=%dx%d src=%p\n"
             "theme=%d emoji_collection=%d count=%d\n",
             txt, (int)cs_hidden, csx, csy, csw, csh,
             pw, ph, cw, ch,
             emo_txt,
             (int)img_hidden, iw, ih, img_src,
             (int)has_theme, (int)has_collection, emoji_count);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return std::string(buf) + "\n(truncated: debug buffer 1024 bytes)";
    }
    return std::string(buf);
}

// ===== 屏幕截图（显存 → 1bpp BMP） =====
//
// 直接读 RLCD 显存而不是抓 LVGL 快照：面板是 1-bit 单色，显存就是屏幕上真实的黑白像素，
// 且只有 15000 字节，导出/传输都很轻。行按 BMP 规范自下而上、4 字节对齐。
bool CustomLcdDisplay::CaptureScreenBmp(uint8_t *out, size_t cap, size_t *out_len) {
    if (rlcd_ == nullptr || out == nullptr || out_len == nullptr) {
        return false;
    }

    const int w = rlcd_->width();
    const int h = rlcd_->height();
    const size_t row_bytes = (size_t)(((w + 31) / 32) * 4);
    const size_t pixel_bytes = row_bytes * (size_t)h;
    const size_t total = 62 + pixel_bytes;
    if (total > cap) {
        ESP_LOGE(TAG, "截图缓冲区不足: 需要 %u, 实际 %u", (unsigned)total, (unsigned)cap);
        return false;
    }

    // P2 #17：把「持锁逐像素读 12 万次」改为「锁内一次性 memcpy 显存快照（约 15KB）→ 解锁后再展开 BMP」。
    // 快照放在堆上（-Werror=frame-larger-than=1536，禁止 15KB 大数组上栈）。
    const size_t snap_len = ((size_t)w * (size_t)h) >> 3;   // 1bpp 显存字节数（400x300 = 15000）
    if (snap_len == 0) {
        ESP_LOGE(TAG, "截图失败: 显存未就绪");
        return false;
    }
    uint8_t *snap = (uint8_t *)heap_caps_malloc(snap_len, MALLOC_CAP_SPIRAM);
    if (snap == nullptr) {
        ESP_LOGE(TAG, "截图失败: 快照缓冲分配失败 (%u 字节)", (unsigned)snap_len);
        return false;
    }
    bool snap_ok = false;
    {
        DisplayLockGuard lock(this);
        snap_ok = rlcd_->CopyDisplayBuffer(snap, snap_len);
    }
    if (!snap_ok) {
        heap_caps_free(snap);
        ESP_LOGE(TAG, "截图失败: 显存快照拷贝失败（驱动未就绪？）");
        return false;
    }

    memset(out, 0, total);

    auto put32 = [out](size_t off, uint32_t v) {
        out[off]     = (uint8_t)(v & 0xFF);
        out[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        out[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        out[off + 3] = (uint8_t)((v >> 24) & 0xFF);
    };
    auto put16 = [out](size_t off, uint16_t v) {
        out[off]     = (uint8_t)(v & 0xFF);
        out[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    };

    // BITMAPFILEHEADER (14 B)
    out[0] = 'B';
    out[1] = 'M';
    put32(2, (uint32_t)total);
    put32(10, 62);                       // 像素数据偏移
    // BITMAPINFOHEADER (40 B)
    put32(14, 40);
    put32(18, (uint32_t)w);
    put32(22, (uint32_t)h);              // 正数 = 自下而上
    put16(26, 1);                        // planes
    put16(28, 1);                        // 1 bpp
    put32(34, (uint32_t)pixel_bytes);
    put32(38, 2835);                     // 72 DPI
    put32(42, 2835);
    put32(46, 2);                        // 调色板 2 色
    put32(50, 2);
    // 调色板：0 = 黑，1 = 白
    out[54] = 0; out[55] = 0; out[56] = 0; out[57] = 0;
    out[58] = 255; out[59] = 255; out[60] = 255; out[61] = 0;

    // 解锁后基于堆快照展开 BMP，避免长时间持有显示锁
    uint8_t *pix = out + 62;
    for (int y = 0; y < h; y++) {
        uint8_t *row = pix + (size_t)(h - 1 - y) * row_bytes;
        for (int x = 0; x < w; x++) {
            if (rlcd_->GetPixelFrom(snap, snap_len, (uint16_t)x, (uint16_t)y)) {
                row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    }
    heap_caps_free(snap);
    *out_len = total;
    return true;
}
