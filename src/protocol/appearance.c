// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-appearance-unstable-v1（观测）+ treeland-appearance-manager-unstable-v1
 * （特权配置）。
 *
 * 两个 XML 是一个功能的两个方向，所以放在同一族里：
 *   观测端  任何客户端都能 bind，bind 之后**立刻**收到全部 11 项当前值，
 *           之后每次变化再收一条——协议原文要求"binding 时推一次"，漏了
 *           这一步客户端只能等下一次变化，界面上就是一片默认色；
 *   配置端  特权客户端（控制中心）发 set_*，合成器立即生效并把新值广播给
 *           所有观测端。
 *
 * 值存在哪儿（这是本文件唯一需要先想清楚的事）：
 *   有渲染消费方的设置写进 `server->config`——cursor / ssd / icon / effect
 *   都只读配置，而项目规定依赖单向（协议层可以调它们，它们不能反向依赖
 *   协议层）。协议族只保留"谁都不消费"的记录（monospace_font /
 *   window_opacity）和诊断计数。
 *   推论：**这些值不落盘**。SIGHUP 重载会按文件重建配置，协议写进去的运行期
 *   值随之回到文件值——配置文件是持久化的唯一来源，协议改的是"此刻"。
 *   重载之后 refresh() 会把新值重播给观测端，控制中心不会看到过期状态。
 *
 * 分档（与 decoration / compositor-action 同一套口径，不假装生效）：
 *   真生效：cursor_theme / cursor_size / font / font_size / icon_theme /
 *           accent_color / color_scheme / titlebar_height / corner_radius；
 *   只记录：monospace_font（woodland 没有等宽字体的消费方）、
 *           window_opacity（没有逐窗口 alpha 通道：要给每个 surface 挂
 *           alpha-modifier 并和 blur/圆角排序，是单独一版）。
 *   两份清单的唯一事实源是 woodland_appearance_settings()：wctl 的
 *   recorded_only 由它生成，单元测试钉住条目数，改档位不会只改一处。
 *
 * 特权判定：协议说"未授权的 bind 可以静默忽略请求甚至断开连接"。woodland
 * 没有 DDE 的 privileged 服务，Wayland 客户端本来就只能是同一个 uid，所以
 * 同 uid 即视为可信——和 compositor_action.c 的取舍一致，写在档位表旁边
 * 而不是藏起来。
 */
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/util/log.h>

#include "config.h"
#include "cursor.h"
#include "effect.h"
#include "icon.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "ssd.h"
#include "text_buffer.h"
#include "trace.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"

#include "treeland-appearance-manager-unstable-v1-protocol.h"
#include "treeland-appearance-unstable-v1-protocol.h"

/* 协议是 experimental：版本号写死 1，上游升版要显式跟 */
#define APPEARANCE_VERSION 1

/* 标题栏高度上界：与 [decorations] titlebar_height 的解析范围一致（<200） */
#define APPEARANCE_TITLEBAR_MAX 200

/* 字号 points*10：5pt..40pt。再小看不清，再大一行标题塞不下 */
#define APPEARANCE_FONT_SIZE_MIN 50
#define APPEARANCE_FONT_SIZE_MAX 400

#define APPEARANCE_COLOR_MAX 255

/* 默认强调色（未设过时）：取默认边框色，和"强调色染激活边框"的用法对齐 */
#define APPEARANCE_DEFAULT_ACCENT_LIGHT 0xAAAAAA
#define APPEARANCE_DEFAULT_ACCENT_DARK 0x3D3D3D

struct appearance_observer {
    struct wl_list link; // woodland_proto_appearance.observers
    struct wl_resource *resource;
};

struct woodland_proto_appearance;

struct appearance_manager {
    struct wl_list link; // woodland_proto_appearance.managers
    struct wl_resource *resource;
    struct woodland_proto_appearance *state;
};

struct woodland_proto_appearance {
    struct woodland_server *server;
    struct wl_global *observer_global;
    struct wl_global *manager_global;
    struct wl_list observers;
    struct wl_list managers;

    /*
     * 只记录、没有渲染消费方的两项。放这里而不是 config：config 是"能画出来的
     * 东西的唯一事实源"，把画不出来的塞进去会让读配置的人以为它生效了。
     */
    char *monospace_font;
    bool opacity_set;
    double window_opacity;

    /*
     * 协议设过的光标尺寸。config 只有单个 size（wlr_xcursor_manager 只吃一个），
     * 但协议要求把 width/height 都报回去；宽高不等时按宽度取 square（height 也
     * 记成宽度），广播的也是这个实际生效值，另记一条 trace 说明降级。
     */
    int32_t cursor_width;
    int32_t cursor_height;

    /* 诊断：最近一次 set_* 被怎么处置，以及累计计数 */
    uint32_t request_count;
    uint32_t applied_count;
    uint32_t recorded_count;
    uint32_t invalid_count;
    const char *last_request;
    const char *last_outcome;
};

/* ==================== 分档表（wctl / 单测的唯一事实源） ==================== */

static const struct woodland_appearance_setting appearance_settings[] = {
    {"set_cursor_theme", WOODLAND_APPEARANCE_APPLIED, "xcursor manager (theme)"},
    {"set_cursor_size", WOODLAND_APPEARANCE_APPLIED, "xcursor manager (size)"},
    {"set_font", WOODLAND_APPEARANCE_APPLIED, "ssd title text (family)"},
    {"set_monospace_font", WOODLAND_APPEARANCE_RECORDED, NULL},
    {"set_font_size", WOODLAND_APPEARANCE_APPLIED, "ssd title text (size)"},
    {"set_icon_theme", WOODLAND_APPEARANCE_APPLIED, "titlebar app icon lookup"},
    {"set_accent_color", WOODLAND_APPEARANCE_APPLIED, "ssd active border"},
    {"set_window_opacity", WOODLAND_APPEARANCE_RECORDED, NULL},
    {"set_color_scheme", WOODLAND_APPEARANCE_APPLIED, "ssd palette"},
    {"set_titlebar_height", WOODLAND_APPEARANCE_APPLIED, "ssd titlebar geometry"},
    /* corner_radius 是条件性消费方：只有 -Deffects=enabled 且运行期
     * [effects] enabled 时才真的画。没渲染后端时值照记、广播报 0、outcome
     * 报 recorded（见 manager_set_corner_radius），不假装已经上屏。 */
    {"set_corner_radius", WOODLAND_APPEARANCE_APPLIED, "scenefx corner radius"},
};

#define APPEARANCE_SETTING_COUNT (sizeof(appearance_settings) / sizeof(appearance_settings[0]))

const struct woodland_appearance_setting *woodland_appearance_settings(size_t *count)
{
    if (count) {
        *count = APPEARANCE_SETTING_COUNT;
    }
    return appearance_settings;
}

const struct woodland_appearance_setting *woodland_appearance_setting_find(const char *request)
{
    if (!request) {
        return NULL;
    }
    for (size_t i = 0; i < APPEARANCE_SETTING_COUNT; i++) {
        if (strcmp(appearance_settings[i].request, request) == 0) {
            return &appearance_settings[i];
        }
    }
    return NULL;
}

/* ==================== 裁剪（纯函数，单测直接调） ==================== */

static uint32_t clamp_uint(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

uint32_t woodland_appearance_clamp_cursor_size(uint32_t value)
{
    return clamp_uint(value, WOODLAND_CURSOR_MIN_SIZE, WOODLAND_CURSOR_MAX_SIZE);
}

uint32_t woodland_appearance_clamp_titlebar_height(uint32_t value)
{
    return clamp_uint(value, 0, APPEARANCE_TITLEBAR_MAX);
}

uint32_t woodland_appearance_clamp_corner_radius(uint32_t value)
{
    return clamp_uint(value, 0, WOODLAND_SSD_MAX_SIZE);
}

uint32_t woodland_appearance_clamp_font_size(uint32_t value)
{
    return clamp_uint(value, APPEARANCE_FONT_SIZE_MIN, APPEARANCE_FONT_SIZE_MAX);
}

/* ==================== 当前值（全部从 config / 族状态现算） ==================== */

static struct woodland_config *appearance_config(struct woodland_proto_appearance *state)
{
    return state && state->server ? state->server->config : NULL;
}

static const char *appearance_cursor_theme(struct woodland_proto_appearance *state)
{
    struct woodland_config *cfg = appearance_config(state);
    if (cfg && cfg->cursor.theme && *cfg->cursor.theme) {
        return cfg->cursor.theme;
    }
    return "default";
}

static void appearance_cursor_size(struct woodland_proto_appearance *state, uint32_t *width,
                                   uint32_t *height)
{
    struct woodland_config *cfg = appearance_config(state);
    int32_t w = state->cursor_width;
    int32_t h = state->cursor_height;
    if (w <= 0) {
        w = (cfg && cfg->cursor.size > 0) ? cfg->cursor.size : WOODLAND_CURSOR_DEFAULT_SIZE;
        h = w;
    }
    *width = (uint32_t)w;
    *height = (uint32_t)(h > 0 ? h : w);
}

static const char *appearance_font_desc(struct woodland_proto_appearance *state)
{
    struct woodland_config *cfg = appearance_config(state);
    return (cfg && cfg->ssd.font && *cfg->ssd.font) ? cfg->ssd.font : "Sans 10";
}

static const char *appearance_font_family(struct woodland_proto_appearance *state)
{
    static char family[128];
    family[0] = '\0';
    if (!woodland_font_split(appearance_font_desc(state), family, sizeof(family), NULL)) {
        snprintf(family, sizeof(family), "%s", "Sans");
    }
    return family;
}

static uint32_t appearance_font_size(struct woodland_proto_appearance *state)
{
    double pt = 0.0;
    if (!woodland_font_split(appearance_font_desc(state), NULL, 0, &pt) || pt <= 0.0) {
        pt = 10.0;
    }
    uint32_t pt10 = (uint32_t)(pt * 10.0 + 0.5);
    return woodland_appearance_clamp_font_size(pt10);
}

static const char *appearance_icon_theme(struct woodland_proto_appearance *state)
{
    struct woodland_config *cfg = appearance_config(state);
    return (cfg && cfg->appearance.icon_theme) ? cfg->appearance.icon_theme : "";
}

static bool appearance_dark(struct woodland_proto_appearance *state)
{
    struct woodland_config *cfg = appearance_config(state);
    return cfg && cfg->appearance.color_scheme_set && cfg->appearance.dark;
}

static void appearance_accent(struct woodland_proto_appearance *state, uint32_t *r, uint32_t *g,
                              uint32_t *b)
{
    struct woodland_config *cfg = appearance_config(state);
    uint32_t rgb = appearance_dark(state) ? APPEARANCE_DEFAULT_ACCENT_DARK
                                          : APPEARANCE_DEFAULT_ACCENT_LIGHT;
    if (cfg && cfg->appearance.accent_set) {
        rgb = cfg->appearance.accent_rgb;
    }
    *r = (rgb >> 16) & 0xFF;
    *g = (rgb >> 8) & 0xFF;
    *b = rgb & 0xFF;
}

static uint32_t appearance_titlebar_height(struct woodland_proto_appearance *state)
{
    struct woodland_config *cfg = appearance_config(state);
    /* 关了服务端装饰就等于没有标题栏：事件报"当前生效值"，而不是配置里的
     * 数字——观测端拿它排版，报一个画不出来的高度只会误导 */
    if (!cfg || !cfg->ssd.enabled) {
        return 0;
    }
    return woodland_appearance_clamp_titlebar_height((uint32_t)cfg->ssd.titlebar_height);
}

/*
 * 圆角有没有真的渲染后端：编译期 -Deffects 决定 scenefx 是否参与链接，
 * 运行期 [effects] enabled 决定画不画，两层都开才会真的画。与 decoration
 * 族的 rendered 列表同一口径——disabled 构建下如实当"只记录"处理。
 */
static bool appearance_corner_rendered(struct woodland_proto_appearance *state)
{
#if WOODLAND_HAVE_EFFECTS
    return woodland_effect_server_enabled(state->server);
#else
    (void)state;
    return false;
#endif
}

static uint32_t appearance_corner_radius(struct woodland_proto_appearance *state)
{
    struct woodland_config *cfg = appearance_config(state);
    if (!cfg || !appearance_corner_rendered(state)) {
        return 0;
    }
    if (cfg->effects.corner_radius < 0) {
        return 0;
    }
    return woodland_appearance_clamp_corner_radius((uint32_t)cfg->effects.corner_radius);
}

/* ==================== 广播 ==================== */

static void broadcast_cursor_theme(struct woodland_proto_appearance *state, const char *name)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_cursor_theme(obs->resource, name);
    }
}

static void broadcast_cursor_size(struct woodland_proto_appearance *state, uint32_t width,
                                  uint32_t height)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_cursor_size(obs->resource, width, height);
    }
}

static void broadcast_font(struct woodland_proto_appearance *state, const char *family)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_font(obs->resource, family);
    }
}

static void broadcast_monospace_font(struct woodland_proto_appearance *state)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_monospace_font(obs->resource,
                                                   state->monospace_font ? state->monospace_font
                                                                         : "");
    }
}

static void broadcast_font_size(struct woodland_proto_appearance *state, uint32_t size)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_font_size(obs->resource, size);
    }
}

static void broadcast_icon_theme(struct woodland_proto_appearance *state, const char *theme)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_icon_theme(obs->resource, theme ? theme : "");
    }
}

static void broadcast_accent_color(struct woodland_proto_appearance *state, uint32_t r, uint32_t g,
                                   uint32_t b)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_accent_color(obs->resource, r, g, b);
    }
}

static void broadcast_window_opacity(struct woodland_proto_appearance *state)
{
    wl_fixed_t value = wl_fixed_from_double(state->opacity_set ? state->window_opacity : 1.0);
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_window_opacity(obs->resource, value);
    }
}

static void broadcast_color_scheme(struct woodland_proto_appearance *state, uint32_t scheme)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_color_scheme(obs->resource, scheme);
    }
}

static void broadcast_titlebar_height(struct woodland_proto_appearance *state, uint32_t height)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_titlebar_height(obs->resource, height);
    }
}

static void broadcast_corner_radius(struct woodland_proto_appearance *state, uint32_t radius)
{
    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        treeland_appearance_v1_send_corner_radius(obs->resource, radius);
    }
}

/* 全量推送：bind 时一次，配置热重载后一次（值可能整体回退） */
static void appearance_send_all(struct woodland_proto_appearance *state,
                                struct wl_resource *resource)
{
    treeland_appearance_v1_send_cursor_theme(resource, appearance_cursor_theme(state));

    uint32_t w, h;
    appearance_cursor_size(state, &w, &h);
    treeland_appearance_v1_send_cursor_size(resource, w, h);

    treeland_appearance_v1_send_font(resource, appearance_font_family(state));
    treeland_appearance_v1_send_monospace_font(resource, state->monospace_font
                                                             ? state->monospace_font
                                                             : "");
    treeland_appearance_v1_send_font_size(resource, appearance_font_size(state));
    treeland_appearance_v1_send_icon_theme(resource, appearance_icon_theme(state));

    uint32_t r, g, b;
    appearance_accent(state, &r, &g, &b);
    treeland_appearance_v1_send_accent_color(resource, r, g, b);

    treeland_appearance_v1_send_window_opacity(
        resource, wl_fixed_from_double(state->opacity_set ? state->window_opacity : 1.0));

    treeland_appearance_v1_send_color_scheme(
        resource, appearance_dark(state) ? TREELAND_APPEARANCE_V1_COLOR_SCHEME_DARK
                                         : TREELAND_APPEARANCE_V1_COLOR_SCHEME_LIGHT);
    treeland_appearance_v1_send_titlebar_height(resource, appearance_titlebar_height(state));
    treeland_appearance_v1_send_corner_radius(resource, appearance_corner_radius(state));
}

/* ==================== 诊断记录 ==================== */

enum appearance_outcome {
    APPEARANCE_OUTCOME_APPLIED,
    APPEARANCE_OUTCOME_CLAMPED,
    APPEARANCE_OUTCOME_RECORDED,
    APPEARANCE_OUTCOME_INVALID,
};

static const char *appearance_outcome_name(enum appearance_outcome outcome)
{
    switch (outcome) {
    case APPEARANCE_OUTCOME_APPLIED:
        return "applied";
    case APPEARANCE_OUTCOME_CLAMPED:
        return "clamped";
    case APPEARANCE_OUTCOME_RECORDED:
        return "recorded";
    case APPEARANCE_OUTCOME_INVALID:
        return "invalid";
    }
    return "unknown";
}

/*
 * 记一条处置结果：wctl 的 last_request/last_outcome 与 --trace 都从这里出。
 *
 * detail 写"依据"而不是重复数值（数值观测端已经收到了）；比如 cursor 宽高
 * 不等时为什么只用了宽度、opacity 为什么只记录。
 */
static void appearance_note(struct woodland_proto_appearance *state, const char *request,
                           enum appearance_outcome outcome, const char *detail)
{
    state->request_count++;
    switch (outcome) {
    case APPEARANCE_OUTCOME_APPLIED:
    case APPEARANCE_OUTCOME_CLAMPED:
        state->applied_count++;
        break;
    case APPEARANCE_OUTCOME_RECORDED:
        state->recorded_count++;
        break;
    case APPEARANCE_OUTCOME_INVALID:
        state->invalid_count++;
        break;
    }
    state->last_request = request;
    state->last_outcome = appearance_outcome_name(outcome);

    if (detail) {
        woodland_trace_add("appearance", 0, "%s -> %s (%s)", request,
                           appearance_outcome_name(outcome), detail);
    } else {
        woodland_trace_add("appearance", 0, "%s -> %s", request,
                           appearance_outcome_name(outcome));
    }
}

/* ==================== manager 请求 ==================== */

static struct woodland_proto_appearance *appearance_from_manager(struct wl_resource *resource)
{
    struct appearance_manager *mgr = resource ? wl_resource_get_user_data(resource) : NULL;
    return mgr ? mgr->state : NULL;
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_set_cursor_theme(struct wl_client *client, struct wl_resource *resource,
                                     const char *name)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    /* 协议原文：空名字退到默认主题（交给 wlroots 的默认主题链） */
    const char *want = (name && *name) ? name : NULL;
    uint32_t w, h;
    appearance_cursor_size(state, &w, &h);
    const char *effective = NULL;
    if (!woodland_cursor_apply_theme(state->server, want, (int)w, &effective)) {
        wlr_log(WLR_ERROR, "appearance: cursor theme '%s' unavailable and no fallback",
                want ? want : "(default)");
        appearance_note(state, "set_cursor_theme", APPEARANCE_OUTCOME_INVALID,
                        "no usable cursor manager");
        return;
    }
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        char *copy = strdup(effective ? effective : "default");
        if (copy) {
            free(cfg->cursor.theme);
            cfg->cursor.theme = copy;
        }
    }
    appearance_note(state, "set_cursor_theme", APPEARANCE_OUTCOME_APPLIED,
                    (want && effective && strcmp(want, effective) != 0)
                        ? "requested theme missing, fell back to default"
                        : NULL);
    broadcast_cursor_theme(state, appearance_cursor_theme(state));
}

static void manager_set_cursor_size(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t width, uint32_t height)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    uint32_t cw = woodland_appearance_clamp_cursor_size(width);
    uint32_t ch = woodland_appearance_clamp_cursor_size(height);
    bool clamped = (cw != width || ch != height);
    /*
     * 宽高不等时只用宽度：wlr_xcursor_manager 的尺寸是单值，非方形光标要等
     * wlroots 支持非方形 cursor plane。协议允许合成器这么做，观测端拿到的
     * 是我们实际用的尺寸，不是它请求的那个。
     */
    const char *effective = NULL;
    if (!woodland_cursor_apply_theme(state->server, appearance_cursor_theme(state), (int)cw,
                                     &effective)) {
        wlr_log(WLR_ERROR, "appearance: cannot resize cursor to %u", cw);
        appearance_note(state, "set_cursor_size", APPEARANCE_OUTCOME_INVALID,
                        "cursor manager unavailable");
        return;
    }
    state->cursor_width = (int32_t)cw;
    state->cursor_height = (int32_t)cw;
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        cfg->cursor.size = (int32_t)cw;
    }
    if (cw != ch) {
        wlr_log(WLR_INFO, "appearance: cursor %ux%u requested, using square %u", cw, ch, cw);
        appearance_note(state, "set_cursor_size", APPEARANCE_OUTCOME_CLAMPED,
                        "non-square cursor unsupported, width used for both");
    } else {
        appearance_note(state, "set_cursor_size",
                        clamped ? APPEARANCE_OUTCOME_CLAMPED : APPEARANCE_OUTCOME_APPLIED,
                        clamped ? "clamped to supported range" : NULL);
    }
    /* 广播实际生效值（square）：观测端拿到的不是它请求的那个，不能把被
     * 降级的 height 原样报回去。 */
    broadcast_cursor_size(state, cw, cw);
}

static void manager_set_font(struct wl_client *client, struct wl_resource *resource,
                             const char *font_name)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    const char *family = (font_name && *font_name) ? font_name : "Sans";
    char desc[256];
    /* 只换族名，字号与字重保持原样：协议没有字重字段，不该顺手抹掉配置里的 */
    if (!woodland_font_with(appearance_font_desc(state), family, 0.0, desc, sizeof(desc))) {
        wlr_log(WLR_INFO, "appearance: font family '%s' too long, ignored", family);
        appearance_note(state, "set_font", APPEARANCE_OUTCOME_INVALID, "font description too long");
        return;
    }
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        char *copy = strdup(desc);
        if (copy) {
            free(cfg->ssd.font);
            cfg->ssd.font = copy;
        }
    }
    woodland_ssd_refresh_all(state->server);
    appearance_note(state, "set_font", APPEARANCE_OUTCOME_APPLIED, NULL);
    broadcast_font(state, family);
}

static void manager_set_monospace_font(struct wl_client *client, struct wl_resource *resource,
                                       const char *font_name)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    char *copy = strdup(font_name ? font_name : "");
    if (!copy) {
        return;
    }
    free(state->monospace_font);
    state->monospace_font = copy;
    /* woodland 不画等宽字体（没有终端/代码视图），但协议要求"改了就说"。
     * 记下来 + 广播 + 明写只记录，客户端才不会以为它生效了。 */
    wlr_log(WLR_INFO, "appearance: monospace font '%s' recorded (no monospace consumer)",
            state->monospace_font[0] ? state->monospace_font : "(empty)");
    appearance_note(state, "set_monospace_font", APPEARANCE_OUTCOME_RECORDED,
                    "no monospace consumer in this compositor");
    broadcast_monospace_font(state);
}

static void manager_set_font_size(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t size)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    uint32_t pt10 = woodland_appearance_clamp_font_size(size);
    char desc[256];
    /* 只换字号，族名与字重不动——这正是 font 与 font_size 是两个请求的意思 */
    if (!woodland_font_with(appearance_font_desc(state), NULL, pt10 / 10.0, desc, sizeof(desc))) {
        appearance_note(state, "set_font_size", APPEARANCE_OUTCOME_INVALID,
                        "font description too long");
        return;
    }
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        char *copy = strdup(desc);
        if (copy) {
            free(cfg->ssd.font);
            cfg->ssd.font = copy;
        }
    }
    woodland_ssd_refresh_all(state->server);
    appearance_note(state, "set_font_size",
                    pt10 != size ? APPEARANCE_OUTCOME_CLAMPED : APPEARANCE_OUTCOME_APPLIED,
                    pt10 != size ? "clamped to supported range" : NULL);
    broadcast_font_size(state, pt10);
}

static void manager_set_icon_theme(struct wl_client *client, struct wl_resource *resource,
                                   const char *theme_name)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    struct woodland_config *cfg = appearance_config(state);
    char *copy = (theme_name && *theme_name) ? strdup(theme_name) : NULL;
    if (theme_name && *theme_name && !copy) {
        return;
    }
    if (cfg) {
        free(cfg->appearance.icon_theme);
        cfg->appearance.icon_theme = copy;
    } else {
        free(copy);
    }
    /* 图标缓存键里带主题，理论上不清也能取对；但旧主题的 buffer 会一直占着
     * 内存（上限 64 项），切几次主题就攒满了，所以显式清一次 */
    woodland_icon_cache_finish();
    woodland_ssd_refresh_all(state->server);
    appearance_note(state, "set_icon_theme", APPEARANCE_OUTCOME_APPLIED,
                    (theme_name && *theme_name) ? NULL : "empty theme, built-in list used");
    broadcast_icon_theme(state, appearance_icon_theme(state));
}

static void manager_set_accent_color(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t r, uint32_t g, uint32_t b)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    /* 协议要求处理越界；这里按分量 clamp 到 255（不是整条丢弃——控制中心
     * 传个 256 就整条不生效，界面上会像"点了没反应"） */
    bool clamped = (r > APPEARANCE_COLOR_MAX || g > APPEARANCE_COLOR_MAX ||
                    b > APPEARANCE_COLOR_MAX);
    if (r > APPEARANCE_COLOR_MAX) {
        r = APPEARANCE_COLOR_MAX;
    }
    if (g > APPEARANCE_COLOR_MAX) {
        g = APPEARANCE_COLOR_MAX;
    }
    if (b > APPEARANCE_COLOR_MAX) {
        b = APPEARANCE_COLOR_MAX;
    }
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        cfg->appearance.accent_set = true;
        cfg->appearance.accent_rgb = (r << 16) | (g << 8) | b;
    }
    woodland_ssd_refresh_all(state->server);
    appearance_note(state, "set_accent_color",
                    clamped ? APPEARANCE_OUTCOME_CLAMPED : APPEARANCE_OUTCOME_APPLIED,
                    clamped ? "component clamped to 255" : NULL);
    broadcast_accent_color(state, r, g, b);
}

static void manager_set_window_opacity(struct wl_client *client, struct wl_resource *resource,
                                       wl_fixed_t opacity)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    double value = wl_fixed_to_double(opacity);
    if (value < 0.0) {
        value = 0.0;
    }
    if (value > 1.0) {
        value = 1.0;
    }
    state->opacity_set = true;
    state->window_opacity = value;
    /* woodland 没有逐窗口 alpha 通道：要让整个窗口半透明，得给每个 surface
     * 挂 alpha-modifier 并和 blur/圆角排序，属单独一版。这里如实记录并明说。 */
    wlr_log(WLR_INFO, "appearance: window opacity %.3f recorded (no per-window alpha path)",
            value);
    appearance_note(state, "set_window_opacity", APPEARANCE_OUTCOME_RECORDED,
                    "no per-window alpha path in this compositor");
    broadcast_window_opacity(state);
}

static void manager_set_color_scheme(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t scheme)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    if (scheme != TREELAND_APPEARANCE_MANAGER_V1_COLOR_SCHEME_LIGHT &&
        scheme != TREELAND_APPEARANCE_MANAGER_V1_COLOR_SCHEME_DARK) {
        /* 协议原文：枚举外的值静默忽略，当前方案保持不变 */
        wlr_log(WLR_INFO, "appearance: color scheme %u is not a valid enum, ignored", scheme);
        appearance_note(state, "set_color_scheme", APPEARANCE_OUTCOME_INVALID,
                        "value outside color_scheme enum");
        return;
    }
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        cfg->appearance.color_scheme_set = true;
        cfg->appearance.dark = (scheme == TREELAND_APPEARANCE_MANAGER_V1_COLOR_SCHEME_DARK);
    }
    woodland_ssd_refresh_all(state->server);
    appearance_note(state, "set_color_scheme", APPEARANCE_OUTCOME_APPLIED,
                    (cfg && cfg->ssd.colors_specified)
                        ? "file-set colors win over the scheme palette"
                        : NULL);
    broadcast_color_scheme(state, scheme);
}

static void manager_set_titlebar_height(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t height)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    uint32_t value = woodland_appearance_clamp_titlebar_height(height);
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        cfg->ssd.titlebar_height = (int32_t)value;
    }
    /*
     * 高度改了边距就变了：SSD 自己按新高度重排，窗口内容原点不动——内容是
     * 以内容原点为基准摆的，标题栏吃掉的是上方空间。这和配置里
     * titlebar_height 热重载的行为一致（那边也只调 ssd 更新，不重排窗口）。
     */
    woodland_ssd_refresh_all(state->server);
    appearance_note(state, "set_titlebar_height",
                    value != height ? APPEARANCE_OUTCOME_CLAMPED : APPEARANCE_OUTCOME_APPLIED,
                    value != height ? "clamped to [0,200]" : NULL);
    broadcast_titlebar_height(state, value);
}

static void manager_set_corner_radius(struct wl_client *client, struct wl_resource *resource,
                                      uint32_t radius)
{
    (void)client;
    struct woodland_proto_appearance *state = appearance_from_manager(resource);
    if (!state) {
        return;
    }
    uint32_t value = woodland_appearance_clamp_corner_radius(radius);
    struct woodland_config *cfg = appearance_config(state);
    if (cfg) {
        cfg->effects.corner_radius = (int32_t)value;
    }
    /* 圆角只改了半径，几何没变；但 CSD 窗口没有 SSD，得逐个让 effect 重算 */
    struct woodland_view *view;
    wl_list_for_each(view, &state->server->views, link)
    {
        woodland_effect_update(view);
    }
    /* 值照记、广播照发（没渲染后端时广播 0）；但 outcome 要如实：没有
     * scenefx 的构建下圆角上不了屏，报 applied 会让控制中心以为已生效。 */
    bool rendered = appearance_corner_rendered(state);
    enum appearance_outcome outcome = rendered
                                         ? (value != radius ? APPEARANCE_OUTCOME_CLAMPED
                                                            : APPEARANCE_OUTCOME_APPLIED)
                                         : APPEARANCE_OUTCOME_RECORDED;
    const char *detail = !rendered
                             ? "no render backend for corners"
                             : (value != radius ? "clamped to [0,1000]" : NULL);
    appearance_note(state, "set_corner_radius", outcome, detail);
    broadcast_corner_radius(state, appearance_corner_radius(state));
}

static const struct treeland_appearance_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .set_cursor_theme = manager_set_cursor_theme,
    .set_cursor_size = manager_set_cursor_size,
    .set_font = manager_set_font,
    .set_monospace_font = manager_set_monospace_font,
    .set_font_size = manager_set_font_size,
    .set_icon_theme = manager_set_icon_theme,
    .set_accent_color = manager_set_accent_color,
    .set_window_opacity = manager_set_window_opacity,
    .set_color_scheme = manager_set_color_scheme,
    .set_titlebar_height = manager_set_titlebar_height,
    .set_corner_radius = manager_set_corner_radius,
};

static void manager_resource_destroy(struct wl_resource *resource)
{
    struct appearance_manager *mgr = wl_resource_get_user_data(resource);
    if (!mgr) {
        return;
    }
    wl_list_remove(&mgr->link);
    free(mgr);
    /* 诊断计数留在族里（wctl 要能回答"上一次谁设的"），这里不碰 */
}

/* ==================== observer 请求 ==================== */

static void observer_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct treeland_appearance_v1_interface observer_impl = {
    .destroy = observer_handle_destroy,
};

static void observer_resource_destroy(struct wl_resource *resource)
{
    struct appearance_observer *obs = wl_resource_get_user_data(resource);
    if (!obs) {
        return;
    }
    wl_list_remove(&obs->link);
    free(obs);
}

/* ==================== bind ==================== */

static void observer_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_appearance *state = data;
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_appearance_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct appearance_observer *obs = calloc(1, sizeof(*obs));
    if (!obs) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    obs->resource = resource;
    wl_resource_set_implementation(resource, &observer_impl, obs, observer_resource_destroy);
    wl_list_insert(&state->observers, &obs->link);

    /* 协议要求 bind 后立刻把 11 项当前值推一遍，客户端不必等下一次变化 */
    appearance_send_all(state, resource);
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_appearance *state = data;
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_appearance_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct appearance_manager *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    mgr->resource = resource;
    mgr->state = state;
    wl_resource_set_implementation(resource, &manager_impl, mgr, manager_resource_destroy);
    wl_list_insert(&state->managers, &mgr->link);

    /* 特权判定：同 uid 即可信（见文件头）。日志留一行，事后能查出谁连过。 */
    wlr_log(WLR_INFO, "treeland-appearance-manager-v1 bound (same-uid client trusted)");
}

/* ==================== 配置热重载后的重播 ==================== */

void woodland_proto_appearance_refresh(struct woodland_proto *proto)
{
    struct woodland_proto_appearance *state = proto ? proto->appearance : NULL;
    if (!state) {
        return;
    }
    /*
     * 重载会把 config 整个换掉：协议设过的光标尺寸（存在族里）已经没有对应
     * 的 config 值了，清掉让 send_all 回到配置里的 size，别报一个合成器
     * 已经不用的数字。
     */
    state->cursor_width = 0;
    state->cursor_height = 0;

    struct appearance_observer *obs;
    wl_list_for_each(obs, &state->observers, link)
    {
        appearance_send_all(state, obs->resource);
    }
}

/* ==================== wctl ==================== */

static void json_recorded_only(struct woodland_json *out)
{
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);
    for (size_t i = 0; i < APPEARANCE_SETTING_COUNT; i++) {
        if (appearance_settings[i].effect != WOODLAND_APPEARANCE_RECORDED) {
            continue;
        }
        woodland_json_arr_sep(&arr);
        woodland_json_string(out, appearance_settings[i].request);
    }
    woodland_json_arr_close(&arr);
}

void woodland_protocol_appearance_status(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_proto_appearance *state = (server && server->proto)
                                                 ? server->proto->appearance
                                                 : NULL;
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    if (!state) {
        woodland_json_obj_bool(&obj, "available", false);
        woodland_json_obj_close(&obj);
        return;
    }
    woodland_json_obj_bool(&obj, "available", true);
    woodland_json_obj_int(&obj, "observer_count", wl_list_length(&state->observers));
    woodland_json_obj_int(&obj, "manager_count", wl_list_length(&state->managers));

    uint32_t w, h;
    appearance_cursor_size(state, &w, &h);
    woodland_json_obj_str(&obj, "cursor_theme", appearance_cursor_theme(state));
    woodland_json_obj_int(&obj, "cursor_width", w);
    woodland_json_obj_int(&obj, "cursor_height", h);

    woodland_json_obj_str(&obj, "font", appearance_font_desc(state));
    woodland_json_obj_str(&obj, "font_family", appearance_font_family(state));
    woodland_json_obj_int(&obj, "font_size_pt10", appearance_font_size(state));
    woodland_json_obj_str(&obj, "monospace_font", state->monospace_font);
    woodland_json_obj_str(&obj, "icon_theme", appearance_icon_theme(state));

    uint32_t r, g, b;
    appearance_accent(state, &r, &g, &b);
    int accent[3] = {(int)r, (int)g, (int)b};
    woodland_json_obj_key(&obj, "accent");
    woodland_json_int_array(out, accent, 3);
    struct woodland_config *cfg = appearance_config(state);
    woodland_json_obj_bool(&obj, "accent_set", cfg && cfg->appearance.accent_set);

    woodland_json_obj_int(&obj, "window_opacity_milli",
                          (long long)((state->opacity_set ? state->window_opacity : 1.0) * 1000.0));
    woodland_json_obj_bool(&obj, "window_opacity_set", state->opacity_set);
    woodland_json_obj_str(&obj, "color_scheme", appearance_dark(state) ? "dark" : "light");
    woodland_json_obj_bool(&obj, "color_scheme_set", cfg && cfg->appearance.color_scheme_set);
    woodland_json_obj_int(&obj, "titlebar_height", appearance_titlebar_height(state));
    woodland_json_obj_int(&obj, "corner_radius", appearance_corner_radius(state));

    /* 哪些设置只记录不渲染：由分档表生成，不手写第二份清单 */
    woodland_json_obj_key(&obj, "recorded_only");
    json_recorded_only(out);

    woodland_json_obj_int(&obj, "request_count", state->request_count);
    woodland_json_obj_int(&obj, "applied_count", state->applied_count);
    woodland_json_obj_int(&obj, "recorded_count", state->recorded_count);
    woodland_json_obj_int(&obj, "invalid_count", state->invalid_count);
    woodland_json_obj_str(&obj, "last_request", state->last_request);
    woodland_json_obj_str(&obj, "last_outcome", state->last_outcome);
    woodland_json_obj_close(&obj);
}

/* ==================== 族初始化 ==================== */

bool woodland_proto_appearance_init(struct woodland_proto *proto)
{
    struct woodland_server *server = proto->server;

    struct woodland_proto_appearance *state = calloc(1, sizeof(*state));
    if (!state) {
        wlr_log(WLR_ERROR, "cannot allocate appearance protocol state");
        return false;
    }
    state->server = server;
    wl_list_init(&state->observers);
    wl_list_init(&state->managers);
    state->last_outcome = "none";
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->appearance = state;

    state->observer_global = wl_global_create(server->display, &treeland_appearance_v1_interface,
                                              APPEARANCE_VERSION, state, observer_bind);
    if (!state->observer_global) {
        wlr_log(WLR_ERROR, "failed to create treeland-appearance global");
        return false;
    }
    state->manager_global = wl_global_create(
        server->display, &treeland_appearance_manager_v1_interface, APPEARANCE_VERSION, state,
        manager_bind);
    if (!state->manager_global) {
        wlr_log(WLR_ERROR, "failed to create treeland-appearance-manager global");
        return false;
    }
    wlr_log(WLR_INFO, "treeland-appearance-v1 and -manager-v1 enabled");
    return true;
}

void woodland_proto_appearance_finish(struct woodland_proto *proto)
{
    struct woodland_proto_appearance *state = proto->appearance;
    if (!state) {
        return;
    }
    /*
     * global 销毁不会带走已 bind 的 wl_resource（见 AGENTS.md 协议陷阱）：
     * 先把族里记着的资源逐个销毁，再释放包装。资源析构会把自己从链表摘掉，
     * 所以这里用 _safe 遍历。
     */
    if (state->observer_global) {
        wl_global_destroy(state->observer_global);
        state->observer_global = NULL;
    }
    if (state->manager_global) {
        wl_global_destroy(state->manager_global);
        state->manager_global = NULL;
    }
    struct appearance_observer *obs, *obs_tmp;
    wl_list_for_each_safe(obs, obs_tmp, &state->observers, link)
    {
        if (obs->resource) {
            wl_resource_set_user_data(obs->resource, NULL);
            wl_resource_destroy(obs->resource);
        }
        wl_list_remove(&obs->link);
        free(obs);
    }
    struct appearance_manager *mgr, *mgr_tmp;
    wl_list_for_each_safe(mgr, mgr_tmp, &state->managers, link)
    {
        if (mgr->resource) {
            wl_resource_set_user_data(mgr->resource, NULL);
            wl_resource_destroy(mgr->resource);
        }
        wl_list_remove(&mgr->link);
        free(mgr);
    }
    free(state->monospace_font);
    free(state);
    proto->appearance = NULL;
}
