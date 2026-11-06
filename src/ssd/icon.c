// SPDX-License-Identifier: GPL-2.0-only
#include "icon.h"
#include "config.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

#include <cairo.h>
#include <ctype.h>
#include <drm_fourcc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if WOODLAND_HAVE_LIBRSVG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#include <librsvg/rsvg.h>
#pragma GCC diagnostic pop
#endif

/* ==================== wlr_buffer 封装 ==================== */

struct icon_wlr_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void icon_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct icon_wlr_buffer *b = wl_container_of(wlr_buffer, b, base);
    if (b->surface) {
        cairo_surface_destroy(b->surface);
    }
    wlr_buffer_finish(&b->base);
    free(b);
}

static bool icon_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
                                              void **data, uint32_t *format, size_t *stride)
{
    (void)flags;
    struct icon_wlr_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_flush(b->surface);
    *data = cairo_image_surface_get_data(b->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(b->surface);
    return true;
}

static void icon_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    struct icon_wlr_buffer *b = wl_container_of(wlr_buffer, b, base);
    cairo_surface_mark_dirty(b->surface);
}

static const struct wlr_buffer_impl icon_buffer_impl = {
    .destroy = icon_buffer_destroy,
    .begin_data_ptr_access = icon_buffer_begin_data_ptr_access,
    .end_data_ptr_access = icon_buffer_end_data_ptr_access,
};

static struct wlr_buffer *wrap_cairo_surface(cairo_surface_t *surf, int width, int height)
{
    if (!surf) {
        return NULL;
    }
    struct icon_wlr_buffer *b = calloc(1, sizeof(*b));
    if (!b) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    b->surface = surf;
    wlr_buffer_init(&b->base, &icon_buffer_impl, width, height);
    return &b->base;
}

/* ==================== 默认回退应用图标绘制 ==================== */

static cairo_surface_t *create_fallback_icon(int size, float scale)
{
    int px = (int)(size * scale);
    if (px <= 0)
        px = 1;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_surface_set_device_scale(surf, scale, scale);
    cairo_t *c = cairo_create(surf);

    /* 绘制现代风格的小窗口轮廓：外框带圆角与顶部标题条 */
    double pad = size * 0.15;
    double w = size - 2 * pad;
    double h = size - 2 * pad;
    double r = size * 0.18;

    /* 窗口底色半透明柔和色 */
    cairo_set_source_rgba(c, 0.25, 0.55, 0.90, 0.18);
    cairo_new_sub_path(c);
    cairo_arc(c, pad + w - r, pad + r, r, -M_PI / 2, 0);
    cairo_arc(c, pad + w - r, pad + h - r, r, 0, M_PI / 2);
    cairo_arc(c, pad + r, pad + h - r, r, M_PI / 2, M_PI);
    cairo_arc(c, pad + r, pad + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(c);
    cairo_fill_preserve(c);

    /* 窗口外框 */
    cairo_set_source_rgba(c, 0.25, 0.55, 0.90, 0.85);
    cairo_set_line_width(c, 1.25);
    cairo_stroke(c);

    /* 顶部小标题栏分隔线 */
    double th = h * 0.35;
    cairo_move_to(c, pad, pad + th);
    cairo_line_to(c, pad + w, pad + th);
    cairo_stroke(c);

    /* 标题栏 3 个小装饰点 */
    double dot_r = size * 0.05;
    double dot_y = pad + th * 0.5;
    cairo_set_source_rgba(c, 0.25, 0.55, 0.90, 0.90);
    for (int i = 0; i < 3; i++) {
        cairo_arc(c, pad + (i + 1) * (w * 0.22), dot_y, dot_r, 0, 2 * M_PI);
        cairo_fill(c);
    }

    cairo_surface_flush(surf);
    cairo_destroy(c);
    return surf;
}

/* ==================== .desktop 文件与 XDG 查找 ==================== */

char *woodland_icon_from_desktop_file(const char *desktop_path)
{
    if (!desktop_path) {
        return NULL;
    }
    FILE *f = fopen(desktop_path, "r");
    if (!f) {
        return NULL;
    }

    char line[512];
    bool in_desktop_entry = false;
    char *found_icon = NULL;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || !*p)
            continue;

        if (*p == '[') {
            in_desktop_entry = (strncmp(p, "[Desktop Entry]", 15) == 0);
            continue;
        }

        if (in_desktop_entry && strncmp(p, "Icon=", 5) == 0) {
            char *val = p + 5;
            while (*val == ' ' || *val == '\t')
                val++;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\r' || val[len - 1] == '\n' ||
                               val[len - 1] == ' ' || val[len - 1] == '\t')) {
                val[--len] = '\0';
            }
            if (*val) {
                found_icon = strdup(val);
                break;
            }
        }
    }

    fclose(f);
    return found_icon;
}

static char *check_desktop_in_dir(const char *dir, const char *app_id)
{
    char path[1024];

    /* 1. <dir>/<app_id>.desktop */
    snprintf(path, sizeof(path), "%s/%s.desktop", dir, app_id);
    if (access(path, R_OK) == 0) {
        return woodland_icon_from_desktop_file(path);
    }

    /* 2. <dir>/<lowercase_app_id>.desktop */
    char lower_id[256];
    size_t i = 0;
    while (app_id[i] && i < sizeof(lower_id) - 1) {
        lower_id[i] = tolower((unsigned char)app_id[i]);
        i++;
    }
    lower_id[i] = '\0';
    snprintf(path, sizeof(path), "%s/%s.desktop", dir, lower_id);
    if (access(path, R_OK) == 0) {
        return woodland_icon_from_desktop_file(path);
    }

    /* 3. 若为反向域名 (如 org.gnome.Terminal)，尝试末尾段 Terminal */
    const char *last_dot = strrchr(app_id, '.');
    if (last_dot && last_dot[1]) {
        snprintf(path, sizeof(path), "%s/%s.desktop", dir, last_dot + 1);
        if (access(path, R_OK) == 0) {
            return woodland_icon_from_desktop_file(path);
        }
        char lower_last[256];
        size_t j = 0;
        while (last_dot[1 + j] && j < sizeof(lower_last) - 1) {
            lower_last[j] = tolower((unsigned char)last_dot[1 + j]);
            j++;
        }
        lower_last[j] = '\0';
        snprintf(path, sizeof(path), "%s/%s.desktop", dir, lower_last);
        if (access(path, R_OK) == 0) {
            return woodland_icon_from_desktop_file(path);
        }
    }

    return NULL;
}

static char *resolve_icon_name_from_desktop(const char *app_id)
{
    if (!app_id || !*app_id) {
        return NULL;
    }

    /* 检索路径列表 */
    const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
    };

    /* 用户本地应用目录 */
    const char *home = getenv("HOME");
    if (home && *home) {
        char user_apps[1024];
        snprintf(user_apps, sizeof(user_apps), "%s/.local/share/applications", home);
        char *icon = check_desktop_in_dir(user_apps, app_id);
        if (icon)
            return icon;
    }

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char *icon = check_desktop_in_dir(dirs[i], app_id);
        if (icon)
            return icon;
    }

    return NULL;
}

static bool check_path_exists(const char *path, char **out)
{
    if (access(path, R_OK) == 0) {
        *out = strdup(path);
        return true;
    }
    return false;
}

/*
 * 在某个图标主题目录里找 target。
 *
 * 先找用户目录再找系统目录（~/.local/share/icons 优先，和 XDG_DATA_DIRS 的
 * 语义一致）；主题名由 treeland-appearance-manager 设，空表示不指定。
 */
static bool find_in_theme(const char *theme, const char *target, char **out)
{
    if (!theme || !*theme) {
        return false;
    }
    static const char *const subdirs[] = {
        "scalable/apps", "48x48/apps", "32x32/apps", "24x24/apps", "16x16/apps",
    };
    char root[512];
    char path[2048];
    const char *home = getenv("HOME");
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0) {
            if (!home || !*home) {
                continue;
            }
            snprintf(root, sizeof(root), "%s/.local/share/icons/%s", home, theme);
        } else if (pass == 1) {
            snprintf(root, sizeof(root), "/usr/share/icons/%s", theme);
        } else {
            snprintf(root, sizeof(root), "/usr/local/share/icons/%s", theme);
        }
        for (size_t s = 0; s < sizeof(subdirs) / sizeof(subdirs[0]); s++) {
            snprintf(path, sizeof(path), "%s/%s/%s.svg", root, subdirs[s], target);
            if (check_path_exists(path, out)) {
                return true;
            }
            snprintf(path, sizeof(path), "%s/%s/%s.png", root, subdirs[s], target);
            if (check_path_exists(path, out)) {
                return true;
            }
            snprintf(path, sizeof(path), "%s/%s/%s", root, subdirs[s], target);
            if (check_path_exists(path, out)) {
                return true;
            }
        }
    }
    return false;
}

/* theme 为 NULL 时就是旧行为：只走 pixmaps + 内置主题表 */
static char *icon_find_path(const char *app_id, const char *theme)
{
    if (!app_id || !*app_id) {
        return NULL;
    }

    /* 1. 先从 .desktop 获取实际 Icon 声明 */
    char *desktop_icon = resolve_icon_name_from_desktop(app_id);
    const char *target = desktop_icon ? desktop_icon : app_id;

    /* 2. 若直接为绝对路径且存在 */
    if (*target == '/') {
        if (access(target, R_OK) == 0) {
            char *ret = strdup(target);
            free(desktop_icon);
            return ret;
        }
    }

    char *result = NULL;
    char path[1024];

    /* 2.5 协议指定的图标主题：它排在通用主题表之前，这样 set_icon_theme
     * 才真的改变图标，而不是"没找到时才有区别" */
    if (find_in_theme(theme, target, &result)) {
        goto done;
    }

    /* 3. 在 /usr/share/pixmaps/ 查找 */
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.svg", target);
    if (check_path_exists(path, &result))
        goto done;
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", target);
    if (check_path_exists(path, &result))
        goto done;
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s", target);
    if (check_path_exists(path, &result))
        goto done;

    /* 4. 在标准主题目录检索 (hicolor / 系统常见主题) */
    static const char *const theme_roots[] = {
        "/usr/share/icons/hicolor",
        "/usr/share/icons/Adwaita",
        "/usr/share/icons/breeze",
        "/usr/share/icons/bloom",
    };

    static const char *const subdirs[] = {
        "scalable/apps", "48x48/apps", "32x32/apps", "24x24/apps", "16x16/apps",
    };

    for (size_t t = 0; t < sizeof(theme_roots) / sizeof(theme_roots[0]); t++) {
        for (size_t s = 0; s < sizeof(subdirs) / sizeof(subdirs[0]); s++) {
            snprintf(path, sizeof(path), "%s/%s/%s.svg", theme_roots[t], subdirs[s], target);
            if (check_path_exists(path, &result))
                goto done;
            snprintf(path, sizeof(path), "%s/%s/%s.png", theme_roots[t], subdirs[s], target);
            if (check_path_exists(path, &result))
                goto done;
            snprintf(path, sizeof(path), "%s/%s/%s", theme_roots[t], subdirs[s], target);
            if (check_path_exists(path, &result))
                goto done;
        }
    }

done:
    free(desktop_icon);
    return result;
}

char *woodland_icon_find_path(const char *app_id)
{
    return icon_find_path(app_id, NULL);
}

/* ==================== 图像加载与渲染 ==================== */

static cairo_surface_t *load_image_surface(const char *filepath, int size, float scale)
{
    if (!filepath) {
        return NULL;
    }
    int px = (int)(size * scale);
    if (px <= 0)
        px = 1;

    size_t len = strlen(filepath);

    /* 1. PNG 文件加载 */
    if (len > 4 && strcasecmp(filepath + len - 4, ".png") == 0) {
        cairo_surface_t *source = cairo_image_surface_create_from_png(filepath);
        if (cairo_surface_status(source) == CAIRO_STATUS_SUCCESS) {
            int sw = cairo_image_surface_get_width(source);
            int sh = cairo_image_surface_get_height(source);
            if (sw > 0 && sh > 0) {
                cairo_surface_t *dest = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
                cairo_surface_set_device_scale(dest, scale, scale);
                cairo_t *cr = cairo_create(dest);
                cairo_scale(cr, (double)size / sw, (double)size / sh);
                cairo_set_source_surface(cr, source, 0, 0);
                cairo_paint(cr);
                cairo_destroy(cr);
                cairo_surface_destroy(source);
                cairo_surface_flush(dest);
                return dest;
            }
        }
        cairo_surface_destroy(source);
    }

    /* 2. SVG 矢量文件加载 */
#if WOODLAND_HAVE_LIBRSVG
    if (len > 4 && strcasecmp(filepath + len - 4, ".svg") == 0) {
        GError *err = NULL;
        RsvgHandle *handle = rsvg_handle_new_from_file(filepath, &err);
        if (handle) {
            cairo_surface_t *dest = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
            cairo_surface_set_device_scale(dest, scale, scale);
            cairo_t *cr = cairo_create(dest);
            RsvgRectangle viewport = {.x = 0, .y = 0, .width = size, .height = size};
            rsvg_handle_render_document(handle, cr, &viewport, NULL);
            cairo_destroy(cr);
            g_object_unref(handle);
            cairo_surface_flush(dest);
            return dest;
        }
        if (err) {
            g_error_free(err);
        }
    }
#endif

    return NULL;
}

/* ==================== 简易 LRU 图标缓存 ==================== */

struct icon_cache_entry {
    struct wl_list link;
    char *app_id;
    /* 图标主题：同一个 app_id 在不同主题下是不同的图，缓存键必须带上它，
     * 否则切主题后老窗口还拿着旧图标（看着像只对了一部分窗口生效） */
    char *theme;
    int size;
    float scale;
    struct wlr_buffer *buffer;
};

static struct wl_list icon_cache = {&icon_cache, &icon_cache};

static void free_cache_entry(struct icon_cache_entry *entry)
{
    wl_list_remove(&entry->link);
    free(entry->app_id);
    free(entry->theme);
    if (entry->buffer) {
        if (!entry->buffer->dropped) {
            wlr_buffer_drop(entry->buffer);
        }
        wlr_buffer_unlock(entry->buffer);
    }
    free(entry);
}

void woodland_icon_cache_finish(void)
{
    struct icon_cache_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &icon_cache, link)
    {
        free_cache_entry(entry);
    }
}

struct wlr_buffer *woodland_ssd_get_icon_buffer(struct woodland_view *view, int size, float scale)
{
    if (!view || size <= 0) {
        return NULL;
    }

    const char *app_id = view->app_id ? view->app_id : view->title;
    if (!app_id) {
        app_id = "default";
    }
    /* 图标主题来自 treeland-appearance-manager（写进 config）；没设过就是 NULL，
     * 走内置主题表的老路径 */
    const char *theme = (view->server && view->server->config)
                            ? view->server->config->appearance.icon_theme
                            : NULL;

    /* 1. 查找缓存 */
    struct icon_cache_entry *entry;
    wl_list_for_each(entry, &icon_cache, link)
    {
        const char *entry_theme = entry->theme ? entry->theme : "";
        const char *want_theme = theme ? theme : "";
        if (entry->size == size && entry->scale == scale && strcmp(entry->app_id, app_id) == 0 &&
            strcmp(entry_theme, want_theme) == 0) {
            return entry->buffer;
        }
    }

    /* 2. 检索文件路径并解码 */
    char *icon_path = icon_find_path(app_id, theme);
    cairo_surface_t *surf = NULL;
    if (icon_path) {
        surf = load_image_surface(icon_path, size, scale);
        free(icon_path);
    }

    /* 3. 未找到或解码失败时绘制优雅回退图标 */
    if (!surf) {
        surf = create_fallback_icon(size, scale);
    }

    struct wlr_buffer *buf = wrap_cairo_surface(surf, size, size);
    if (!buf) {
        return NULL;
    }

    /* 4. 存入缓存（限制最多 64 项） */
    if (wl_list_length(&icon_cache) >= 64) {
        struct icon_cache_entry *oldest = wl_container_of(icon_cache.prev, oldest, link);
        free_cache_entry(oldest);
    }

    struct icon_cache_entry *new_entry = calloc(1, sizeof(*new_entry));
    if (new_entry) {
        new_entry->app_id = strdup(app_id);
        new_entry->theme = theme ? strdup(theme) : NULL;
        new_entry->size = size;
        new_entry->scale = scale;
        new_entry->buffer = wlr_buffer_lock(buf);
        wl_list_insert(&icon_cache, &new_entry->link);
    }

    return buf;
}
