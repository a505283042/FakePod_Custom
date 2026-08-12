#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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

// 配置 PCM 路径、0.5Vrms 满量程和默认 -20dB 数字音量，
// 按数据手册 pop-free 序列开启耳放；返回时 PCM 仍保持静音。
esp_err_t cs43131_prepare_headphone_playback_low_volume();

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
