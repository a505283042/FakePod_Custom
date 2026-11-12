#include "picture_app.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include <string>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_jpeg_dec.h"
#include "png.h"

#include "lvgl.h"
#include "app_manager.h"
#include "font/font_manager.h"
#include "ui/gesture/gesture_router.h"

static const char *TAG = "图片APP";

static constexpr uint16_t kScreenWidth = 460;
static constexpr uint16_t kScreenHeight = 460;
static constexpr int kMaxFiles = 500;

enum class PictureState : uint8_t {
    Stopped = 0,
    Browsing,
    Viewing,
};

static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_image = nullptr;
static lv_obj_t *g_status = nullptr;
static lv_obj_t *g_counter = nullptr;
static lv_timer_t *g_timer = nullptr;
static PictureState g_state = PictureState::Stopped;

static std::vector<std::string> g_files;
static int g_current = -1;
static uint8_t *g_rgb565 = nullptr;
static uint16_t g_img_width = 0;
static uint16_t g_img_height = 0;
static lv_image_dsc_t g_img_dsc = {};

// ---------- 文件扫描 ----------

static bool has_image_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr) return false;
    return strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0;
}

static void scan_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == nullptr) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (g_files.size() >= kMaxFiles) break;

        char full[512];
        const int written = snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_directory(full);
        } else if (S_ISREG(st.st_mode) && has_image_extension(entry->d_name)) {
            g_files.emplace_back(full);
        }
    }
    closedir(dir);
}

static void scan_files()
{
    g_files.clear();
    scan_directory("/sdcard");
    std::sort(g_files.begin(), g_files.end());
    ESP_LOGI(TAG, "扫描到 %u 张图片", static_cast<unsigned>(g_files.size()));
}

// ---------- 解码 ----------

static void free_image()
{
    if (g_rgb565 != nullptr) {
        heap_caps_free(g_rgb565);
        g_rgb565 = nullptr;
    }
    g_img_width = 0;
    g_img_height = 0;
}

static bool decode_jpeg(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "JPEG 打开失败: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        ESP_LOGW(TAG, "JPEG 文件大小异常: %s size=%ld", path, size);
        fclose(f); return false;
    }

    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGW(TAG, "JPEG 文件缓冲分配失败: %s size=%ld", path, size);
        fclose(f); return false;
    }
    const size_t read = fread(buf, 1, size, f);
    fclose(f);
    if (read != static_cast<size_t>(size)) {
        ESP_LOGW(TAG, "JPEG 读取不完整: %s read=%u/%ld", path, static_cast<unsigned>(read), size);
        heap_caps_free(buf); return false;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == nullptr) {
        ESP_LOGW(TAG, "JPEG open 失败: %s", path);
        heap_caps_free(buf); return false;
    }

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = buf;
    io.inbuf_len = static_cast<int>(size);

    jpeg_error_t ret = jpeg_dec_parse_header(decoder, &io, &info);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG header 解析失败: %s ret=%d", path, static_cast<int>(ret));
        jpeg_dec_close(decoder); heap_caps_free(buf); return false;
    }

    const uint16_t sw = info.width;
    const uint16_t sh = info.height;
    const size_t out_size = static_cast<size_t>(sw) * sh * 2U;
    g_rgb565 = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16U, out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_rgb565 == nullptr) {
        ESP_LOGW(TAG, "JPEG 输出缓冲分配失败: %s size=%u", path, static_cast<unsigned>(out_size));
        jpeg_dec_close(decoder); heap_caps_free(buf); return false;
    }

    io.outbuf = g_rgb565;
    io.out_size = static_cast<int>(out_size);
    ret = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    heap_caps_free(buf);

    if (ret != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG 解码失败: %s ret=%d", path, static_cast<int>(ret));
        heap_caps_free(g_rgb565); g_rgb565 = nullptr; return false;
    }
    g_img_width = sw;
    g_img_height = sh;
    return true;
}

static bool decode_png(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "PNG 打开失败: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 8 * 1024 * 1024) {
        ESP_LOGW(TAG, "PNG 文件大小异常: %s size=%ld", path, size);
        fclose(f); return false;
    }

    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        ESP_LOGW(TAG, "PNG 文件缓冲分配失败: %s size=%ld", path, size);
        fclose(f); return false;
    }
    const size_t read = fread(buf, 1, size, f);
    fclose(f);
    if (read != static_cast<size_t>(size)) {
        ESP_LOGW(TAG, "PNG 读取不完整: %s read=%u/%ld", path, static_cast<unsigned>(read), size);
        heap_caps_free(buf); return false;
    }

    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, buf, size)) {
        ESP_LOGW(TAG, "PNG header 解析失败: %s size=%ld msg=%s", path, size, image.message);
        heap_caps_free(buf); return false;
    }
    image.format = PNG_FORMAT_RGB;
    const size_t out_size = static_cast<size_t>(image.width) * image.height * 3U;
    uint8_t *rgb888 = static_cast<uint8_t *>(heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (rgb888 == nullptr) {
        ESP_LOGW(TAG, "PNG RGB888 缓冲分配失败: %s w=%u h=%u", path,
            static_cast<unsigned>(image.width), static_cast<unsigned>(image.height));
        png_image_free(&image); heap_caps_free(buf); return false;
    }

    png_color background = {};
    if (!png_image_finish_read(&image, &background, rgb888, 0, nullptr)) {
        ESP_LOGW(TAG, "PNG 解码失败: %s msg=%s", path, image.message);
        heap_caps_free(rgb888); png_image_free(&image); heap_caps_free(buf); return false;
    }
    png_image_free(&image);
    heap_caps_free(buf);

    const uint16_t sw = static_cast<uint16_t>(image.width);
    const uint16_t sh = static_cast<uint16_t>(image.height);
    const size_t rgb565_size = static_cast<size_t>(sw) * sh * 2U;
    g_rgb565 = static_cast<uint8_t *>(heap_caps_malloc(rgb565_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_rgb565 == nullptr) {
        ESP_LOGW(TAG, "PNG RGB565 缓冲分配失败: %s", path);
        heap_caps_free(rgb888); return false;
    }

    for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i) {
        const uint8_t r = rgb888[i * 3];
        const uint8_t g = rgb888[i * 3 + 1];
        const uint8_t b = rgb888[i * 3 + 2];
        const uint16_t pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        g_rgb565[i * 2] = pixel & 0xFF;
        g_rgb565[i * 2 + 1] = pixel >> 8;
    }
    heap_caps_free(rgb888);
    g_img_width = sw;
    g_img_height = sh;
    return true;
}

static bool decode_image(const char *path)
{
    free_image();
    const char *dot = strrchr(path, '.');
    if (dot == nullptr) return false;
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return decode_jpeg(path);
    }
    if (strcasecmp(dot, ".png") == 0) {
        return decode_png(path);
    }
    return false;
}

// ---------- 显示 ----------

static void show_image()
{
    if (g_current < 0 || g_current >= static_cast<int>(g_files.size())) return;
    if (!decode_image(g_files[g_current].c_str())) {
        ESP_LOGW(TAG, "解码失败: %s", g_files[g_current].c_str());
        if (g_status != nullptr) lv_label_set_text(g_status, "解码失败");
        return;
    }

    // 计算缩放比例，保持宽高比，居中显示
    const float sx = static_cast<float>(kScreenWidth) / g_img_width;
    const float sy = static_cast<float>(kScreenHeight) / g_img_height;
    const float scale = std::min(sx, sy);
    const uint16_t dw = static_cast<uint16_t>(g_img_width * scale);
    const uint16_t dh = static_cast<uint16_t>(g_img_height * scale);
    const int16_t ox = (kScreenWidth - dw) / 2;
    const int16_t oy = (kScreenHeight - dh) / 2;

    // 创建缩放后的 RGB565 缓冲区
    const size_t out_size = static_cast<size_t>(dw) * dh * 2U;
    uint8_t *scaled = static_cast<uint8_t *>(heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM));
    if (scaled == nullptr) return;

    // 双线性插值缩放
    for (uint16_t y = 0; y < dh; ++y) {
        for (uint16_t x = 0; x < dw; ++x) {
            const float fx = static_cast<float>(x) / scale;
            const float fy = static_cast<float>(y) / scale;
            const uint16_t ix = static_cast<uint16_t>(fx);
            const uint16_t iy = static_cast<uint16_t>(fy);

            const size_t i00 = (static_cast<size_t>(iy) * g_img_width + ix) * 2;

            const uint8_t lo = static_cast<uint8_t>(g_rgb565[i00]);
            const uint8_t hi = static_cast<uint8_t>(g_rgb565[i00 + 1]);
            scaled[(static_cast<size_t>(y) * dw + x) * 2] = lo;
            scaled[(static_cast<size_t>(y) * dw + x) * 2 + 1] = hi;
        }
    }

    free_image();
    g_rgb565 = scaled;
    g_img_width = dw;
    g_img_height = dh;

    g_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    g_img_dsc.header.w = g_img_width;
    g_img_dsc.header.h = g_img_height;
    g_img_dsc.data_size = out_size;
    g_img_dsc.data = g_rgb565;

    if (g_image != nullptr) {
        lv_image_set_src(g_image, &g_img_dsc);
        lv_obj_set_pos(g_image, ox, oy);
        lv_obj_clear_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_status != nullptr) lv_obj_add_flag(g_status, LV_OBJ_FLAG_HIDDEN);

    if (g_counter != nullptr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%u", g_current + 1, static_cast<unsigned>(g_files.size()));
        lv_label_set_text(g_counter, buf);
        lv_obj_clear_flag(g_counter, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_status(const char *text)
{
    if (g_status != nullptr) {
        lv_label_set_text(g_status, text);
        lv_obj_clear_flag(g_status, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_image != nullptr) lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    if (g_counter != nullptr) lv_obj_add_flag(g_counter, LV_OBJ_FLAG_HIDDEN);
}

// ---------- 手势 ----------

static void handle_gesture()
{
    UiGestureAction action = UiGestureAction::None;
    if (!gesture_router_take_action(&action)) return;

    if (g_state == PictureState::Viewing) {
        if (action == UiGestureAction::SwipeLeft) {
            // 下一张
            if (g_current + 1 < static_cast<int>(g_files.size())) {
                ++g_current;
                show_image();
            }
        } else if (action == UiGestureAction::SwipeRight) {
            // 上一张
            if (g_current > 0) {
                --g_current;
                show_image();
            }
        } else if (action == UiGestureAction::PullDownFromTop) {
            // 返回 Launcher
            (void)app_manager_request_foreground(AppId::Music, AppTransitionMode::Exclusive);
        }
    } else if (g_state == PictureState::Browsing) {
        if (action == UiGestureAction::SwipeLeft || action == UiGestureAction::SwipeRight ||
            action == UiGestureAction::SingleTap) {
            if (!g_files.empty()) {
                g_state = PictureState::Viewing;
                g_current = 0;
                show_image();
            }
        } else if (action == UiGestureAction::PullDownFromTop) {
            (void)app_manager_request_foreground(AppId::Music, AppTransitionMode::Exclusive);
        }
    }
}

// ---------- 生命周期 ----------

static esp_err_t picture_create()
{
    if (g_root != nullptr) return ESP_OK;
    lv_obj_t *screen = lv_screen_active();
    if (screen == nullptr) return ESP_ERR_INVALID_STATE;

    g_root = lv_obj_create(screen);
    if (g_root == nullptr) return ESP_ERR_NO_MEM;
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_root, kScreenWidth, kScreenHeight);
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_hex(0x0A0E14), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    g_image = lv_image_create(g_root);
    if (g_image != nullptr) {
        lv_obj_add_flag(g_image, LV_OBJ_FLAG_HIDDEN);
    }

    g_status = lv_label_create(g_root);
    if (g_status != nullptr) {
        lv_obj_set_style_text_font(g_status, font_manager_get_ui_font(), 0);
        lv_obj_set_width(g_status, kScreenWidth - 40);
        lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(g_status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(g_status, lv_color_hex(0x8899AA), 0);
        lv_obj_align(g_status, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(g_status, "正在扫描…");
    }

    g_counter = lv_label_create(g_root);
    if (g_counter != nullptr) {
        lv_obj_set_style_text_font(g_counter, font_manager_get_ui_font(), 0);
        lv_obj_set_style_text_color(g_counter, lv_color_hex(0x667788), 0);
        lv_obj_align(g_counter, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_add_flag(g_counter, LV_OBJ_FLAG_HIDDEN);
    }

    g_timer = lv_timer_create([](lv_timer_t *) { handle_gesture(); }, 20, nullptr);
    if (g_timer == nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(g_timer);

    scan_files();
    if (g_files.empty()) {
        show_status("未找到图片\n请将 JPG/PNG 放入 SD 卡");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "共 %u 张图片\n左右滑动翻页", static_cast<unsigned>(g_files.size()));
        show_status(buf);
    }
    g_state = PictureState::Browsing;
    return ESP_OK;
}

static esp_err_t picture_enter()
{
    if (g_root == nullptr) return ESP_ERR_INVALID_STATE;
    lv_obj_clear_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    gesture_router_reset();
    lv_timer_resume(g_timer);
    return ESP_OK;
}

static esp_err_t picture_leave(AppRunState next_state)
{
    if (g_timer != nullptr) lv_timer_pause(g_timer);
    if (g_root != nullptr) lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    free_image();
    g_state = PictureState::Browsing;
    return ESP_OK;
}

static void picture_destroy()
{
    if (g_timer != nullptr) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    if (g_root != nullptr) {
        lv_obj_delete(g_root);
        g_root = nullptr;
    }
    g_image = nullptr;
    g_status = nullptr;
    g_counter = nullptr;
    free_image();
    g_files.clear();
    g_state = PictureState::Stopped;
}

esp_err_t picture_app_register()
{
    AppDescriptor descriptor = {};
    descriptor.id = AppId::Picture;
    descriptor.name = "图片";
    descriptor.supports_background = false;
    descriptor.lifecycle.create = picture_create;
    descriptor.lifecycle.enter = picture_enter;
    descriptor.lifecycle.leave = picture_leave;
    descriptor.lifecycle.destroy = picture_destroy;
    const esp_err_t ret = app_manager_register(descriptor);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "图片APP已注册");
    }
    return ret;
}
