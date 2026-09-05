#pragma once

#include "freertos/FreeRTOS.h"

#include "esp_err.h"

// Stage 12.1：所有本地 TF/FATFS 访问统一通过一把递归互斥锁串行化。
// 递归锁允许同一任务的高层扫描/事务函数持锁后，再进入底层 helper 继续访问 SD，避免自锁死锁。
esp_err_t storage_io_init();
bool storage_io_is_ready();

bool storage_sd_lock(TickType_t timeout_ticks = portMAX_DELAY);
void storage_sd_unlock();

// USB MSC 运行时接管分两阶段：
// 1) begin 先等所有既有 SD 临界区退出，再封锁普通任务的新访问，并由调用任务独占总线；
// 2) finish 成功时保留封锁但释放互斥锁给 USB raw owner，失败回滚时解除封锁。
bool storage_io_begin_usb_handoff(TickType_t timeout_ticks);
void storage_io_finish_usb_handoff(bool keep_blocked);
bool storage_io_usb_handoff_blocked();

// V3：TinyUSB 已释放 raw SD owner 后，先由归还任务重新成为唯一 owner；
// 在 /sdcard 重挂载完成前仍保持 blocked，成功后再一次性放开所有应用侧访问。
bool storage_io_begin_usb_return(TickType_t timeout_ticks);
void storage_io_finish_usb_return(bool app_storage_ready);

// C++ RAII guard。默认永久等待；运行时低优先级资产任务可传有限超时，避免异常情况下长期阻塞自身。
class StorageSdLockGuard
{
public:
    explicit StorageSdLockGuard(TickType_t timeout_ticks = portMAX_DELAY)
        : locked_(storage_sd_lock(timeout_ticks))
    {
    }

    ~StorageSdLockGuard()
    {
        if (locked_) {
            storage_sd_unlock();
        }
    }

    StorageSdLockGuard(const StorageSdLockGuard &) = delete;
    StorageSdLockGuard &operator=(const StorageSdLockGuard &) = delete;

    bool locked() const
    {
        return locked_;
    }

    explicit operator bool() const
    {
        return locked_;
    }

private:
    bool locked_ = false;
};
