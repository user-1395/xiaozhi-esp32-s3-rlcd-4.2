#ifndef __RLCD_DRIVER_H__
#define __RLCD_DRIVER_H__

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>

// LUT 行跨度：取最大边长 400，横屏(400x300)/竖屏(300x400) 都不会越界
#define RLCD_LUT_STRIDE 400

// RLCD 像素颜色（1-bit 单色屏，只有黑白两色）
enum ColorSelection {
    ColorBlack = 0,    
    ColorWhite = 0xff
};

// SPI 显示屏引脚配置
typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

// RLCD 硬件驱动层
// 
// 负责底层 SPI 通信、像素映射、屏幕初始化等硬件操作。
// 这个类不涉及任何 UI 逻辑，只提供像素级别的绘制能力。
class RlcdDriver {
public:
    RlcdDriver(spi_display_config_t spiconfig, int width, int height, 
               spi_host_device_t spi_host = SPI3_HOST);
    ~RlcdDriver();

    // 屏幕初始化（发送初始化命令序列）。返回 false 表示显存/LUT 未就绪
    // （PSRAM 分配失败），此时所有绘制调用会安全降级，不再 assert/panic。
    bool RLCD_Init();
    // 清屏（填充指定颜色）
    void RLCD_ColorClear(uint8_t color);
    // 设置单个像素；返回该像素对应的显存字节是否真的发生了变化
    bool RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
    // 将整帧缓冲区刷新到屏幕（横屏 15000 B）
    void RLCD_Display();
    // 局部刷新：只把 x∈[x1,x2] 的竖直条带送到屏幕（x 自动对齐到 2 像素对）
    // 仅横屏（width > height）支持；竖屏 LUT 为行优先，退化为整帧刷新
    void RLCD_DisplayXRange(uint16_t x1, uint16_t x2);
    // 设置屏幕对比度 (0x00-0x1F)
    void SetContrast(uint8_t level);
    // 软件反色：true=深底白字，false=浅底黑字。
    // v10 起改为「送显位流按位取反」实现，不再发 0x21 硬件反色命令——
    // 硬件反色会改变面板驱动极性，实测导致左上角边缘像素闪烁；软件反色保持
    // 面板极性恒为 0x20（正常），与浅色完全一致的驱动波形，故不会闪。
    void SetInversion(bool enabled);
    bool inversion() const { return software_invert_.load(); }

    // 获取屏幕尺寸
    int width() const { return width_; }
    int height() const { return height_; }

    // 传输统计（用于评估局部刷新效果：累计字节数 / 传输次数 / 整帧字节数）
    uint64_t transferred_bytes() const { return transferred_bytes_; }
    uint32_t transfer_count() const { return transfer_count_; }
    int display_len() const { return DisplayLen; }

    // 把整块 1-bit 显存快照拷贝到 dst（截图用：调用方持显示锁时做一次 memcpy，
    // 避免逐像素持锁遍历）。cap 需 >= display_len()，成功返回 true。
    bool CopyDisplayBuffer(uint8_t *dst, size_t cap) const;
    // 从外部显存快照读取像素（true = 白）；越界/未就绪返回 false。
    bool GetPixelFrom(const uint8_t *buffer, size_t len, uint16_t x, uint16_t y) const;
    // 显存/LUT 是否分配成功（失败时驱动安全降级）
    bool ready() const { return init_ok_; }

    // 读取显存中的像素（true = 白，false = 黑），用于把当前画面导出成图片。
    // 入口做 x/y 边界检查：越界或显存未就绪时返回 false（避免越界读 LUT/显存）。
    bool RLCD_GetPixel(uint16_t x, uint16_t y) const {
        if (DispBuffer == nullptr || PixelIndexLUT == nullptr || PixelBitLUT == nullptr) {
            return false;
        }
        if (x >= (uint16_t)width_ || y >= (uint16_t)height_) {
            return false;
        }
        return (DispBuffer[PixelIndexLUT[x][y]] & PixelBitLUT[x][y]) != 0;
    }

    // 获取 SPI IO 句柄（LVGL 初始化可能需要）
    esp_lcd_panel_io_handle_t io_handle() const { return io_handle_; }

private:
    // SPI 引脚配置
    int mosi_;
    int scl_;
    int dc_;
    int cs_;
    int rst_;
    int width_;
    int height_;

    // SPI IO 句柄
    esp_lcd_panel_io_handle_t io_handle_ = nullptr;

    // 1-bit 显示缓冲区（渲染结果，浅极性语义：bit=1 白 / bit=0 黑）
    uint8_t *DispBuffer = nullptr;
    int DisplayLen = 0;
    // 软件反色送显临时缓冲（DispBuffer 逐字节取反后送到面板，不改渲染结果/截图）
    uint8_t *InvertBuffer = nullptr;
    std::atomic<bool> software_invert_{false};
    // 显存/LUT 是否分配成功；false 时所有绘制调用安全降级（不 panic）
    bool init_ok_ = false;

    // 传输统计
    uint64_t transferred_bytes_ = 0;
    uint32_t transfer_count_ = 0;

    // 像素映射查找表（加速 RGB565 → 1-bit 转换）
    // 行跨度固定为 RLCD_LUT_STRIDE，保证竖屏(300x400)时 y 方向不越界；缓冲区按 width*stride 分配
    uint16_t (*PixelIndexLUT)[RLCD_LUT_STRIDE] = nullptr;
    uint8_t  (*PixelBitLUT)[RLCD_LUT_STRIDE] = nullptr;

    // 初始化像素映射 LUT（竖屏/横屏两种模式）
    void InitPortraitLUT();
    void InitLandscapeLUT();

    // 硬件控制
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t *Data, int len);
    void RLCD_Reset();
};

#endif
