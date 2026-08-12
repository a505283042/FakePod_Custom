[1mdiff --git a/src/boot/boot_state.cpp b/src/boot/boot_state.cpp[m
[1mindex d37ffc0..055091c 100644[m
[1m--- a/src/boot/boot_state.cpp[m
[1m+++ b/src/boot/boot_state.cpp[m
[36m@@ -58,9 +58,29 @@[m [mvoid boot_state_init()[m
         APP_DIAG_PROFILE_NAME[m
     );[m
 [m
[31m-#if APP_DIAG_BOOT_VERBOSE[m
[31m-    ESP_LOGI(TAG, "启动保护等待：5000ms（串口监视器窗口）");[m
[31m-#endif[m
[32m+[m[32m    // 串口监视器不再阻塞正式启动；调试连接由主机侧自行完成。[m
[32m+[m[32m    // 旧 WaitStart 暂留兼容，正常入口直接进入第一个真实启动阶段。[m
[32m+[m[32m    g_state = BootState::CheckPsram;[m
[32m+[m[32m}[m
[32m+[m
[32m+[m
[32m+[m[32mBootRunResult boot_run()[m
[32m+[m[32m{[m
[32m+[m[32m    if (boot_state_has_error()) {[m
[32m+[m[32m        return BootRunResult::Fatal;[m
[32m+[m[32m    }[m
[32m+[m[32m    if (boot_state_is_ready()) {[m
[32m+[m[32m        return BootRunResult::Ready;[m
[32m+[m[32m    }[m
[32m+[m
[32m+[m[32m    boot_state_update();[m
[32m+[m
[32m+[m[32m    if (boot_state_has_error()) {[m
[32m+[m[32m        return BootRunResult::Fatal;[m
[32m+[m[32m    }[m
[32m+[m[32m    return boot_state_is_ready()[m
[32m+[m[32m        ? BootRunResult::Ready[m
[32m+[m[32m        : BootRunResult::Running;[m
 }[m
 [m
 [m
