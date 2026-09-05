#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum class Cs43131OutputProfile : uint8_t {
    NormalHeadphones = 0,   // 0.5 Vrms, HV_EN=0
    HighImpedance = 1,     // 1.41 Vrms, HV_EN=0；覆盖完整耳机负载范围
    LineOut = 2,           // 2.0 Vrms, HV_EN=1；仅用于高阻线路负载
};

// 初始化 CS43131 控制口并验证芯片身份；初始化完成后保持音频模块安全掉电。
esp_err_t cs43131_init();

// 读取/写入 8 位 CS43131 寄存器，寄存器地址为 24 位 MAP 地址。
esp_err_t cs43131_read_reg(uint32_t address, uint8_t *value);
esp_err_t cs43131_write_reg(uint32_t address, uint8_t value);

// 正式 PCM 播放接口：配置板载 24.576MHz XTAL 与 ASP。
// 当前 Rate Profile 覆盖 44.1~192kHz；I2S 使用 32bit slot，CS43131 为 Slave。
esp_err_t cs43131_prepare_pcm_playback_32bit(uint32_t sample_rate_hz);

// 只启动 ASP 数字输入路径，耳放保持关闭。
esp_err_t cs43131_enable_asp_input();

// 读取 ASP 中断状态 2，用于检查 LRCK early/late/no-LRCK 等错误。
esp_err_t cs43131_read_asp_status(uint8_t *status);

// 配置 PCM 路径、指定模拟满量程和默认 -20dB 数字音量，
// 按数据手册 pop-free 序列开启耳放；返回时 PCM 仍保持静音。
// profile 只会在 PDN_HP=1 时写入 OUT_FS / HV_EN / +1dB_EN。
esp_err_t cs43131_prepare_headphone_playback(Cs43131OutputProfile profile);

// 运行期安全切换模拟输出档：函数只负责 PDN_HP -> 改档 -> pop-free 上电，ASP/I2S 保持运行。
// 调用者必须先软静音并保持零 PCM；返回后仍保持调用前设置的 PCM mute 状态。
esp_err_t cs43131_switch_headphone_output_profile(Cs43131OutputProfile profile);

// 读取当前 PCM 手动静音位（A/B 任一静音都返回 true）。
esp_err_t cs43131_get_pcm_mute(bool *muted);

// 设置左右声道 PCM 数字衰减，单位为 0.5dB steps：0=0dB，40=-20dB。
// 调用者负责确保该值处于芯片允许范围；运行期仅 AudioTask 调用。
esp_err_t cs43131_set_pcm_volume_attenuation(uint8_t half_db_steps);

// PCM 软斜坡静音/解除静音。
esp_err_t cs43131_set_pcm_mute(bool mute);

// 按 PDN_DONE 序列关闭耳放与 ASP，XTAL 暂时保持运行。
esp_err_t cs43131_power_down_headphone_playback();

// 关闭 ASP/XTAL 并恢复控制口待机状态。
esp_err_t cs43131_finish_pcm_playback();

bool cs43131_is_ready();
uint8_t cs43131_get_revision();
uint8_t cs43131_get_subrevision();
