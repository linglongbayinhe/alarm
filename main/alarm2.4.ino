/*
ESP32                                                          MAX98357
D22                       ->                                     DIN
D15                       ->                                     LRC
D14                       ->                                     BCLK
VIN                       ->                                     VIN
GND                       ->                                     GND
*/

#include "BluetoothA2DPSink.h"
#include <driver/i2s.h> 
BluetoothA2DPSink a2dp_sink;

//-------------------------- I2S 硬件配置 --------------------------
// 1. I2S 引脚（根据你的硬件接线修改，此处为常用推荐引脚）
const i2s_port_t I2S_PORT = I2S_NUM_0; // 使用 I2S 端口 0
i2s_pin_config_t i2s_pins = {
.bck_io_num = 14, // I2S_BCK（时钟线）
.ws_io_num = 15, // I2S_WS（声道选择线 / LRCLK）
.data_out_num = 19, // I2S_DATA（数据线）
.data_in_num = I2S_PIN_NO_CHANGE // 无需输入，设为 “无变化”
};

// 2. I2S 格式配置（A2DP 默认输出 44.1kHz、16 位、立体声）
i2s_config_t i2s_config = {
.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), // 主机模式 + 发送模式
.sample_rate = 44100, // 采样率（与 A2DP 音频一致）
.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // 16 位采样（A2DP 标准）
.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // 立体声
.communication_format = I2S_COMM_FORMAT_STAND_I2S, // 标准 I2S 协议
.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // 低优先级中断（避免抢占蓝牙）
.dma_buf_count = 2, // DMA 缓冲区数量（2 个足够，减少内存占用）
.dma_buf_len = 256, // 每个缓冲区长度（256 字节，平衡延迟与稳定性）
.use_apll = false // 禁用 APLL 时钟（避免与蓝牙时钟冲突）
};

//-------------------------- 函数声明 --------------------------
void init_i2s_hardware (); // 初始化 I2S 硬件
void audio_data_handler (const uint8_t*, uint32_t); // 音频数据回调处理

//TFT 1.90
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();

//DS3231 时钟
#include <RTClib.h>
#include <Wire.h>
RTC_DS3231 rtc;          // 创建RTC对象

void setup () {
  //音频模块
  Serial.begin (115200);
  delay (1000); // 启动延时，确保硬件初始化完成
  Serial.println ("=== 初始化 A2DP 蓝牙 + I2S 音频系统 ===");
  // 1. 先初始化 I2S 硬件（自主控制，避开库内部冲突）
  init_i2s_hardware ();
  // 2. 配置 A2DP：设置音频回调，禁用库内部 I2S（用我们自己的 I2S）
  a2dp_sink.set_stream_reader (audio_data_handler, false);
  // 3. 启动 A2DP 蓝牙服务（设备名可自定义，手机搜索此名称连接）
  a2dp_sink.start ("ESP32-Bluetooth-Speaker");
  Serial.println ("✅ A2DP 服务启动完成，手机蓝牙搜索：ESP32-Bluetooth-Speaker");
  Serial.println ("📌 连接后播放音乐，即可通过 I2S 输出声音");

  // 初始化屏幕
  tft.init();
  tft.setRotation(1); // 设置屏幕方向（0-3，根据实际显示方向调整）
  Serial.println("TFT init completed.");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);   // 开启背光
  // 将屏幕背景设置为红色
  tft.fillScreen(TFT_RED);
  Serial.println("Screen filled with RED.");
  // 在屏幕上打印一行字
  // // 1. 设置文字的对齐方式为“中心对齐”
  // tft.setTextDatum(MC_DATUM); // MC_DATUM 代表 Middle Center Datum
  // tft.drawString("Screen OK!", tft.width() / 2, tft.height() / 2, 4); // 在屏幕中央显示 "Screen OK!"
  

  delay(100);
  //时钟
  Wire.begin(21, 22); // 显式指定 SDA, SCL
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    while(1);
  }
  // 固定标签（只画一次）
  tft.setTextColor(TFT_WHITE); // 设置字体为白色
  tft.setTextSize(2);        // 设置文字大小（2倍）
  tft.setCursor(10, 40);
  tft.print("Date: ");
  tft.setCursor(10, 100);
  tft.print("Time: ");
}

void loop () {
// 主循环无需额外操作，音频处理在回调中异步执行
  ShowTime_DS3231();
  delay(1000);
}

//-------------------------- 函数实现 --------------------------
// 初始化 I2S 硬件（自主控制，稳定性更高）
void init_i2s_hardware () {
// 1. 安装 I2S 驱动
esp_err_t err = i2s_driver_install (I2S_PORT, &i2s_config, 0, NULL);
if (err == ESP_OK) {
Serial.println ("✅ I2S 驱动安装成功");
} else {
Serial.print ("❌ I2S 驱动安装失败，错误码：");
Serial.println (err);
}

// 2. 设置 I2S 引脚
i2s_set_pin (I2S_PORT, &i2s_pins);
Serial.println ("✅ I2S 引脚配置完成");

// 3. 清空 I2S 缓冲区（避免启动时的杂音）
i2s_zero_dma_buffer (I2S_PORT);
}

// 音频数据回调：A2DP 接收的音频数据会自动调用此函数
void audio_data_handler (const uint8_t *audio_data, uint32_t data_len) {
// 仅在数据有效时发送到 I2S（避免空指针）
if (audio_data != NULL && data_len > 0) {
size_t bytes_written; // 记录实际写入 I2S 的字节数
// 阻塞式写入（portMAX_DELAY：无限等待，确保数据不丢失）
i2s_write (I2S_PORT, audio_data, data_len, &bytes_written, portMAX_DELAY);
}
}

//时钟
void ShowTime_DS3231()
{
  DateTime now = rtc.now();
  Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second());

  // 日期字符串（固定格式：YYYY-MM-DD）
  char dateStr[11];   // 10字符 + 结束符
  sprintf(dateStr, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  // 时间字符串（固定格式：HH:MM:SS）
  char timeStr[9];    // 8字符 + 结束符
  sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  // 设置文字颜色（背景色与屏幕底色相同，避免残留）
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.setTextSize(2);

  tft.setCursor(70, 40);   // 与标签“Date: ”对齐
  tft.print(dateStr);
  tft.setCursor(70, 100);  // 与标签“Time: ”对齐
  tft.print(timeStr);
}