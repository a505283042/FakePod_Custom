#include "font_manager.h"
#include "storage_io.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdcard.h"
#include "app_diag_config.h"

static const char *TAG = "字体";

#if APP_DIAG_BOOT_VERBOSE
#define FONT_BOOT_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define FONT_BOOT_LOGI(...) APP_DIAG_DISCARDED_LOGI(TAG, __VA_ARGS__)
#endif

// ============================================================
// FakePod 原厂字体格式
// ============================================================
//
// 文件头，共 12 字节：
//
// 0x00  uint16  字符范围下界（不包含）
// 0x02  uint16  字符范围上界（包含）
// 0x04  uint32  位图 BPP
// 0x08  uint32  缺字占位字形偏移
//
// 0x0C 开始是 Unicode -> 字形偏移表，每项 uint32 小端。
// 表项位置：0x0C + (unicode - lower - 1) * 4
//
// 字形记录：
// byte 0  advance width
// byte 1  bitmap width
// byte 2  bitmap height
// byte 3  x offset，int8
// byte 4  字形底部到整行底部的距离（bottom bearing）
// byte 5  保留，目前文件中为 0
// byte 6  开始为 2bpp 位图
//
// 2bpp 位图每行单独按字节对齐，每字节从高位到低位存 4 个像素。
// ============================================================

static constexpr const char *FONT_PATH = "/sdcard/FONTS/SYHT_BOLD_24.bin";
static constexpr size_t FONT_HEADER_SIZE = 12;
static constexpr size_t GLYPH_HEADER_SIZE = 6;
static constexpr uint32_t EXPECTED_BPP = 2;

struct OriginalFontContext
{
    uint8_t *data;
    size_t size;
    uint16_t lower_exclusive;
    uint16_t upper_inclusive;
    uint32_t bpp;
    uint32_t missing_glyph_offset;
    int32_t line_height;
    int32_t base_line;
    int32_t baseline_y;
};

static OriginalFontContext g_context = {};
static lv_font_t g_ui_font = {};
static bool g_ready = false;

static uint16_t font_manager_read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t font_manager_read_le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

static uint32_t font_manager_lookup_glyph_offset(uint32_t unicode)
{
    if (!g_ready && g_context.data == nullptr) {
        return 0;
    }

    if (unicode <= g_context.lower_exclusive || unicode > g_context.upper_inclusive) {
        return 0;
    }

    const uint32_t index = unicode - g_context.lower_exclusive - 1;
    const size_t table_offset = FONT_HEADER_SIZE + static_cast<size_t>(index) * sizeof(uint32_t);
    if (table_offset + sizeof(uint32_t) > g_context.size) {
        return 0;
    }

    const uint32_t glyph_offset = font_manager_read_le32(g_context.data + table_offset);
    if (glyph_offset == 0 || glyph_offset == g_context.missing_glyph_offset ||
        static_cast<size_t>(glyph_offset) + GLYPH_HEADER_SIZE > g_context.size) {
        return 0;
    }

    return glyph_offset;
}

static bool font_manager_get_glyph_dsc_cb(
    const lv_font_t *font,
    lv_font_glyph_dsc_t *dsc_out,
    uint32_t unicode_letter,
    uint32_t unicode_letter_next)
{
    (void)font;
    (void)unicode_letter_next;

    bool is_tab = false;
    if (unicode_letter == '\t') {
        unicode_letter = ' ';
        is_tab = true;
    }

    const uint32_t glyph_offset = font_manager_lookup_glyph_offset(unicode_letter);
    if (glyph_offset == 0) {
        // 正文不使用 LVGL fallback；返回 false 让 LVGL 按“本字体缺字”处理。
        // Symbol 图标仍由页面显式选择 lv_font_default()。
        return false;
    }

    const uint8_t *glyph = g_context.data + glyph_offset;
    const uint8_t advance_width = glyph[0];
    const uint8_t box_width = glyph[1];
    const uint8_t box_height = glyph[2];
    const int8_t offset_x = static_cast<int8_t>(glyph[3]);
    // 字体记录 byte[4] 是“字形底部到行底的距离”(bottom bearing)，
    // 不是从行顶开始的 top_y。对齐 LVGL 时应转换成相对基线的 ofs_y。
    // 例如本字体：A/a 的 bottom bearing=8，g=2；因此 g 会自然下伸 6px。
    const uint8_t bottom_bearing = glyph[4];

    dsc_out->adv_w = is_tab ? static_cast<uint16_t>(advance_width) * 2 : advance_width;
    dsc_out->box_w = box_width;
    dsc_out->box_h = box_height;
    dsc_out->ofs_x = offset_x;
    dsc_out->ofs_y = static_cast<int32_t>(bottom_bearing) - g_context.base_line;
    dsc_out->format = LV_FONT_GLYPH_FORMAT_A2;
    dsc_out->is_placeholder = false;
    dsc_out->gid.index = glyph_offset;

    return true;
}

static const void *font_manager_get_glyph_bitmap_cb(
    lv_font_glyph_dsc_t *glyph_dsc,
    lv_draw_buf_t *draw_buf)
{
    if (glyph_dsc == nullptr || draw_buf == nullptr || draw_buf->data == nullptr) {
        return nullptr;
    }

    if (glyph_dsc->box_w == 0 || glyph_dsc->box_h == 0) {
        return nullptr;
    }

    const size_t glyph_offset = static_cast<size_t>(glyph_dsc->gid.index);
    if (glyph_offset + GLYPH_HEADER_SIZE > g_context.size) {
        return nullptr;
    }

    const size_t row_bytes = (static_cast<size_t>(glyph_dsc->box_w) * EXPECTED_BPP + 7) / 8;
    const size_t packed_size = row_bytes * static_cast<size_t>(glyph_dsc->box_h);
    const size_t bitmap_offset = glyph_offset + GLYPH_HEADER_SIZE;
    if (bitmap_offset + packed_size > g_context.size) {
        return nullptr;
    }

    const uint32_t stride = lv_draw_buf_width_to_stride(glyph_dsc->box_w, LV_COLOR_FORMAT_A8);
    const size_t output_size = static_cast<size_t>(stride) * glyph_dsc->box_h;
    if (output_size > draw_buf->data_size) {
        ESP_LOGE(TAG, "LVGL 字形缓冲不足：需要=%u，可用=%u",
            static_cast<unsigned>(output_size),
            static_cast<unsigned>(draw_buf->data_size));
        return nullptr;
    }

    static constexpr uint8_t OPACITY_2BPP[4] = {0, 85, 170, 255};
    const uint8_t *packed = g_context.data + bitmap_offset;
    memset(draw_buf->data, 0, output_size);

    for (uint32_t y = 0; y < glyph_dsc->box_h; ++y) {
        const uint8_t *source_row = packed + static_cast<size_t>(y) * row_bytes;
        uint8_t *target_row = draw_buf->data + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < glyph_dsc->box_w; ++x) {
            const uint8_t packed_byte = source_row[x >> 2];
            const uint8_t shift = static_cast<uint8_t>(6 - ((x & 0x03U) * 2));
            const uint8_t level = static_cast<uint8_t>((packed_byte >> shift) & 0x03U);
            target_row[x] = OPACITY_2BPP[level];
        }
    }

    return draw_buf;
}





static esp_err_t font_manager_analyze_metrics()
{
    uint32_t valid_glyphs = 0;
    uint32_t max_vertical_extent = 0;

    const uint32_t table_count = g_context.upper_inclusive - g_context.lower_exclusive;
    for (uint32_t index = 0; index < table_count; ++index) {
        const size_t table_offset = FONT_HEADER_SIZE + static_cast<size_t>(index) * sizeof(uint32_t);
        const uint32_t glyph_offset = font_manager_read_le32(g_context.data + table_offset);
        if (glyph_offset == 0 ||
            glyph_offset == g_context.missing_glyph_offset ||
            static_cast<size_t>(glyph_offset) + GLYPH_HEADER_SIZE > g_context.size) {
            continue;
        }

        const uint8_t *glyph = g_context.data + glyph_offset;
        // byte[4] 是 bottom bearing，因此 bearing + box_h 是该字形从行底向上的总占用。
        const uint32_t vertical_extent = static_cast<uint32_t>(glyph[4]) + glyph[2];
        if (vertical_extent > max_vertical_extent) {
            max_vertical_extent = vertical_extent;
        }
        valid_glyphs++;
    }

    if (valid_glyphs == 0 || max_vertical_extent == 0) {
        ESP_LOGE(TAG, "字体中没有有效字形");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // 用明确落在拉丁基线上的大写字母和数字推断 base_line。
    // 不对全字体做众数统计，因为大量 CJK 字形会把基线误判成汉字的 bottom bearing。
    static constexpr uint32_t BASELINE_ANCHORS[] = {
        'A', 'H', 'M', 'N', 'X',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'
    };
    uint16_t baseline_histogram[256] = {};
    uint16_t anchor_count = 0;
    for (uint32_t unicode : BASELINE_ANCHORS) {
        const uint32_t glyph_offset = font_manager_lookup_glyph_offset(unicode);
        if (glyph_offset == 0 ||
            static_cast<size_t>(glyph_offset) + GLYPH_HEADER_SIZE > g_context.size) {
            continue;
        }
        const uint8_t bottom_bearing = g_context.data[glyph_offset + 4];
        baseline_histogram[bottom_bearing]++;
        anchor_count++;
    }

    if (anchor_count == 0) {
        ESP_LOGE(TAG, "无法从 ASCII 锚点推断字体基线");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t base_line = 0;
    uint16_t baseline_votes = 0;
    for (uint32_t y = 0; y < 256; ++y) {
        if (baseline_histogram[y] > baseline_votes) {
            baseline_votes = baseline_histogram[y];
            base_line = y;
        }
    }

    if (base_line >= max_vertical_extent) {
        ESP_LOGE(TAG, "字体基线异常：base_line=%lu line_height=%lu",
            static_cast<unsigned long>(base_line),
            static_cast<unsigned long>(max_vertical_extent));
        return ESP_ERR_INVALID_RESPONSE;
    }

    g_context.line_height = static_cast<int32_t>(max_vertical_extent);
    g_context.base_line = static_cast<int32_t>(base_line);
    g_context.baseline_y = g_context.line_height - g_context.base_line;

    FONT_BOOT_LOGI(
        "字体度量分析：有效字形=%lu，行高=%ld，base_line=%ld，baseline_y=%ld，ASCII锚点=%u票/%u",
        static_cast<unsigned long>(valid_glyphs),
        static_cast<long>(g_context.line_height),
        static_cast<long>(g_context.base_line),
        static_cast<long>(g_context.baseline_y),
        static_cast<unsigned>(baseline_votes),
        static_cast<unsigned>(anchor_count));

    return ESP_OK;
}

static esp_err_t font_manager_validate_format()
{
    if (g_context.size < FONT_HEADER_SIZE) {
        ESP_LOGE(TAG, "字体文件过小");
        return ESP_ERR_INVALID_SIZE;
    }

    g_context.lower_exclusive = font_manager_read_le16(g_context.data);
    g_context.upper_inclusive = font_manager_read_le16(g_context.data + 2);
    g_context.bpp = font_manager_read_le32(g_context.data + 4);
    g_context.missing_glyph_offset = font_manager_read_le32(g_context.data + 8);

    if (g_context.lower_exclusive >= g_context.upper_inclusive) {
        ESP_LOGE(TAG, "字体 Unicode 范围无效");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (g_context.bpp != EXPECTED_BPP) {
        ESP_LOGE(TAG, "暂不支持该字体 BPP：%lu", static_cast<unsigned long>(g_context.bpp));
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t table_count = static_cast<size_t>(g_context.upper_inclusive - g_context.lower_exclusive);
    const size_t table_end = FONT_HEADER_SIZE + table_count * sizeof(uint32_t);
    if (table_end > g_context.size) {
        ESP_LOGE(TAG, "字体 Unicode 索引表越界");
        return ESP_ERR_INVALID_SIZE;
    }

    if (static_cast<size_t>(g_context.missing_glyph_offset) + GLYPH_HEADER_SIZE > g_context.size) {
        ESP_LOGE(TAG, "字体缺字字形偏移无效：0x%08lX",
            static_cast<unsigned long>(g_context.missing_glyph_offset));
        return ESP_ERR_INVALID_SIZE;
    }

    FONT_BOOT_LOGI("原厂字体格式确认：Unicode U+%04X~U+%04X，%lu bpp",
        static_cast<unsigned>(g_context.lower_exclusive + 1),
        static_cast<unsigned>(g_context.upper_inclusive),
        static_cast<unsigned long>(g_context.bpp));
    FONT_BOOT_LOGI("Unicode 索引表：%u 项，缺字字形偏移=0x%08lX",
        static_cast<unsigned>(table_count),
        static_cast<unsigned long>(g_context.missing_glyph_offset));

    return ESP_OK;
}

esp_err_t font_manager_init()
{
    if (g_ready) {
        return ESP_OK;
    }

    if (!sdcard_is_mounted()) {
        ESP_LOGE(TAG, "TF 卡未挂载，无法加载中文字体");
        return ESP_ERR_INVALID_STATE;
    }

    struct stat info = {};
    {
        StorageSdLockGuard sd_lock;
        if (!sd_lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        if (stat(FONT_PATH, &info) != 0 || info.st_size <= 0) {
            ESP_LOGE(TAG, "中文字体不存在：%s", FONT_PATH);
            return ESP_ERR_NOT_FOUND;
        }
    }

    const size_t font_size = static_cast<size_t>(info.st_size);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint8_t *font_data = static_cast<uint8_t *>(
        heap_caps_malloc(font_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (font_data == nullptr) {
        ESP_LOGE(TAG, "申请字体 PSRAM 失败：需要=%u 字节", static_cast<unsigned>(font_size));
        return ESP_ERR_NO_MEM;
    }

    size_t read_count = 0U;
    {
        StorageSdLockGuard sd_lock;
        if (!sd_lock.locked()) {
            heap_caps_free(font_data);
            return ESP_ERR_TIMEOUT;
        }
        FILE *file = fopen(FONT_PATH, "rb");
        if (file == nullptr) {
            heap_caps_free(font_data);
            ESP_LOGE(TAG, "打开中文字体失败：%s", FONT_PATH);
            return ESP_FAIL;
        }

        FONT_BOOT_LOGI("正在将中文字体载入 PSRAM：%s", FONT_PATH);
        read_count = fread(font_data, 1, font_size, file);
        fclose(file);
    }
    if (read_count != font_size) {
        heap_caps_free(font_data);
        ESP_LOGE(TAG, "读取中文字体不完整：期望=%u，实际=%u",
            static_cast<unsigned>(font_size),
            static_cast<unsigned>(read_count));
        return ESP_FAIL;
    }

    g_context.data = font_data;
    g_context.size = font_size;
    esp_err_t ret = font_manager_validate_format();
    if (ret != ESP_OK) {
        heap_caps_free(g_context.data);
        g_context = {};
        return ret;
    }

    ret = font_manager_analyze_metrics();
    if (ret != ESP_OK) {
        heap_caps_free(g_context.data);
        g_context = {};
        return ret;
    }

    g_ui_font = {};
    g_ui_font.get_glyph_dsc = font_manager_get_glyph_dsc_cb;
    g_ui_font.get_glyph_bitmap = font_manager_get_glyph_bitmap_cb;
    g_ui_font.release_glyph = nullptr;
    g_ui_font.line_height = g_context.line_height;
    g_ui_font.base_line = g_context.base_line;
    g_ui_font.subpx = LV_FONT_SUBPX_NONE;
    g_ui_font.kerning = LV_FONT_KERNING_NONE;
    g_ui_font.underline_position = -2;
    g_ui_font.underline_thickness = 1;
    g_ui_font.dsc = &g_context;
    // 所有正文/菜单只使用原厂中文字体；Symbol 图标由页面显式使用 lv_font_default()。
    // 不再让正文缺字时偷偷切换成 LVGL 西文字体。
    g_ui_font.fallback = nullptr;
    g_ui_font.user_data = nullptr;

    g_ready = true;
    const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "中文字体初始化成功：文件=%u KB，行高=%ld，基线=%ld",
        static_cast<unsigned>(font_size / 1024),
        static_cast<long>(g_context.line_height),
        static_cast<long>(g_context.base_line));
    FONT_BOOT_LOGI("字体占用 PSRAM：%u KB，剩余=%u KB",
        static_cast<unsigned>((psram_before - psram_after) / 1024),
        static_cast<unsigned>(psram_after / 1024));

    return ESP_OK;
}

const lv_font_t *font_manager_get_ui_font()
{
    return g_ready ? &g_ui_font : lv_font_default();
}

bool font_manager_is_ready()
{
    return g_ready;
}
