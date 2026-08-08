#pragma once

#include "freertos/FreeRTOS.h"

#include "esp_err.h"

// Stage 12.1：所有本地 TF/FATFS 访问统一通过一把递归互斥锁串行化。
// 递归锁允许同一任务的高层扫描/事务函数持锁后，再进入底层 helper 继续访问 SD，避免自锁死锁。
esp_err_t storage_io_init();
bool storage_io_is_ready();

bool storage_sd_lock(TickType_t timeout_ticks = portMAX_DELAY);
void storage_sd_unlock();

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
