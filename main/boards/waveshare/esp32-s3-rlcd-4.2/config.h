#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
// 自包含所需类型/常量头（P2 #22），避免依赖包含顺序（不改变任何数值）
#include <driver/i2c_types.h>    // I2C_NUM_0
#include <driver/spi_common.h>   // SPI3_HOST
#include <es8311_codec.h>        // ES8311_CODEC_DEFAULT_ADDR
#include <es7210_adc.h>          // ES7210_CODEC_DEFAULT_ADDR

#define ESP32_I2C_HOST      I2C_NUM_0
#define ESP32_LCD_HOST      SPI3_HOST

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_13
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

// ===== 天气 =====
// 优先使用和风天气（QWeather）：可拿到实时（体感/风/湿度/能见度/气压）+ 空气质量 +
// 3 天预报 + 日出日落 + 官方预警；凭证为空时自动回退免密钥源（中国天气网 → Open-Meteo）。
//
// 凭证填写优先级：NVS("weather"/"api_host","api_key") > managers/weather_qweather_secret.h > 这里
// secret 头文件已 gitignore，严禁把真实密钥写在本文件里。
#define QWEATHER_HOST ""   // 例："xxxxxx.re.qweatherapi.com"（控制台项目管理里的 API Host）
#define QWEATHER_KEY  ""   // 例：32 位十六进制 API KEY

// 免密钥源参数（和风不可用时的兜底）
#define WEATHER_LAT   39.9042f    // 纬度（默认北京；切换城市时按城市参数表动态取）
#define WEATHER_LON   116.4074f   // 经度
#define WEATHER_CITY  "北京"       // 查询/显示的城市名（和风 geo 定位也用它）
#define WEATHER_CITY_CODE "101010100"

// IP 定位（ip-api.com 免费接口，HTTP，45 次/分钟）：仅作最后兜底。
// 注意：IP 定位得到的是「出口 IP」的注册地，设备在 Windows 热点/运营商 NAT 后面时常常与真实
// 所在地不一致（实测出口 IP 判为南京、实际在常州），因此手动配置城市（NVS weather/city 或
// MCP self.weather.set_city）优先，IP 定位只在没配置时使用。
#define WEATHER_IP_GEO_URL "http://ip-api.com/json/?lang=zh-CN&fields=status,country,regionName,city,lat,lon"
// 备用 IP 定位源（主源失败时依次尝试，字段名各不相同，解析层已兼容）
//   ipapi.co:  city / region / latitude / longitude（失败时含 error 字段）
//   ipinfo.io: city / region / loc="lat,lon"
#define WEATHER_IP_GEO_URL_ALT1 "https://ipapi.co/json/"
#define WEATHER_IP_GEO_URL_ALT2 "https://ipinfo.io/json"

// 后端天气地址接口（xiaozhi-server 的 GET /xiaozhi/weather）。
// 设备优先从后端拿「实际所在地 + 和风 LocationID」，不再用出口 IP 判城市；
// 后端不可用时回退下面的 WEATHER_CITY，IP 定位仅作最后兜底。后端位置由 config.yaml 的
// plugins.get_weather.device_location 配置。
#define WEATHER_BACKEND_GEO_URL ""

// 待办 MCP 接入点（websocket JSON-RPC）。留空表示不启用，可在运行时通过
// MCP 工具 self.todo.set_url 配置（持久化在 NVS namespace "todo", key "mcp_url"）。
// 例：wss://example.com:59826/mcp_endpoint/call/?token=xxx
#define MCP_TODO_URL ""

// ===== 电池检测 =====
// 采样：ADC1 通道 3（ESP32-S3 上 ADC1_CH3 = GPIO4），12 dB 衰减、12 bit、硬件曲线拟合校准。
// 待补充（需原理图/实测确认）：采样引脚、分压比、是否有充电 IC 的 STAT 引脚接到 SoC。
// 分压比若与原理图不符，只改 BATTERY_ADC_DIVIDER 即可（电池电压 = ADC 引脚电压 × 该系数）。
#define BATTERY_ADC_DIVIDER   3.0f    // 分压系数（实测校准：4.2V 电池对应 ADC ≈1.40V）
#define BATTERY_ADC_SAMPLES   16      // 过采样次数（去最大最小后平均）
#define BATTERY_LOAD_COMP_MV  30.0f   // 放电时的负载压降补偿（mV），充电时不补偿
#define BATTERY_FULL_MV       4180    // 满电/接 USB 平台电压（持续≥该值视为充电/已充满）
// 待补充（需硬件规格确认）：电池标称容量与整机平均电流。填 0 时界面/接口只显示百分比，
// 不输出「剩余 xx mAh / 预计可用 xx 小时」，避免用假设值误导。
#define BATTERY_CAPACITY_MAH  0       // 例如 2000（单节锂电）
#define BATTERY_AVG_CURRENT_MA 0      // 例如 120（整机平均放电电流）

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define USER_BUTTON_GPIO        GPIO_NUM_18  // 用户按键（屏幕右侧 KEY 按钮）


#define RLCD_DC_PIN    GPIO_NUM_5  
#define RLCD_CS_PIN    GPIO_NUM_40
#define RLCD_SCK_PIN   GPIO_NUM_11
#define RLCD_MOSI_PIN  GPIO_NUM_12
#define RLCD_RST_PIN   GPIO_NUM_41
#define RLCD_TE_PIN    GPIO_NUM_6

// 屏幕方向：0 = 横屏 400x300（默认，2x2 卡片网格）；1 = 竖屏 300x400（单列堆叠）
// 改这一个宏即可切换，重新编译烧录生效
#define RLCD_PORTRAIT 0

#if RLCD_PORTRAIT
#define RLCD_WIDTH   300
#define RLCD_HEIGHT  400
#else
#define RLCD_WIDTH   400
#define RLCD_HEIGHT  300
#endif

#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#endif 
