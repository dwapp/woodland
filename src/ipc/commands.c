// SPDX-License-Identifier: GPL-2.0-only
/*
 * 调试接口的命令层：一条命令 -> 一段 JSON。
 *
 * 这一层不认识 socket，也不认识事件循环：输入是一个字符串，输出是一个
 * woodland_json 缓冲。好处是它可以在单元测试里直接调，不用起监听、
 * 不用连客户端；换传输方式（比如以后加个 fifo）也不用改这里。
 *
 * 全部命令只读。给"设置几何""注入按键"这类写命令开的口子，需要和正常
 * 输入路径抢状态，一个 bug 就变成"合成器被调试工具搞坏了"，收益不值。
 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include "scene.h"
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

#include "config.h"
#include "effect.h"
#include "focus.h"
#include "ipc.h"
#include "output.h"
#include "placement.h"
#include "selfcheck.h"
#include "ssd.h"
#include "trace.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"

/* 场景树转储的递归深度与节点上限：防止一次 dump 输出几百 KB */
#define SCENE_MAX_DEPTH 6
#define SCENE_MAX_NODES 2000

/* ==================== 命令文档：help 的唯一事实源 ==================== */

/*
 * help 的自描述表。加新命令时在这里补一份字段清单，help 的输出就自动
 * 跟上——调用方（脚本或 AI）不需要读 commands.c 就知道能问什么、
 * 每个字段是什么类型。desc 用英文短句：它会被 grep/jq 消费。
 */
struct ipc_field_doc {
    const char *name;
    const char *type;
    const char *desc;
};

struct ipc_cmd_doc {
    const char *name;
    const char *desc;
    const struct ipc_field_doc *fields;
    size_t nfields;
};

/* 字段数从数组大小算，不手数——手数迟早数错 */
#define IPC_FIELDS_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

static const struct ipc_field_doc ping_fields[] = {
    {"pid", "int", "compositor process id"},
};

static const struct ipc_field_doc get_tree_fields[] = {
    {"pid", "int", "compositor process id"},
    {"ipc", "str", "debug socket path"},
    {"placement_policy", "str", "cascade | center | cursor | autoplace"},
    {"self_check", "bool", "--self-check was given at startup"},
    {"effects", "bool", "[effects] enabled && blur: runtime blur capability"},
    {"outputs", "obj[]",
     "name, enabled, box[x,y,w,h], workarea, width, "
     "height, scale (per-mille)"},
    {"workspace", "obj", "current: str, all: str[]"},
    {"focus", "obj", "same fields as get_focus"},
    {"views", "obj[]",
     "per view: id, kind, z, title, app_id, tag, workspace, "
     "output, mapped, placed, minimized, maximized, "
     "fullscreen, override_redirect, csd, focused, "
     "activated, focusable, scene_visible, parent, "
     "outer[x,y,w,h], content[x,y,w,h], "
     "ssd{present,active,margins}, "
     "deco{managed,titlebar,border_width,border_rgba,"
     "corner_radius,shadow_radius,shadow_offset,"
     "shadow_rgba,rendered[]}, "
     "bg_effect{blur,region[x,y,w,h]}, limits, snapped[]"},
    {"count", "int", "number of views"},
};

static const struct ipc_field_doc get_focus_fields_doc[] = {
    {"view", "int|null", "focused view id (recorded focus)"},
    {"recorded_view", "int|null", "id the compositor tracks as focused"},
    {"keyboard_view", "int|null", "view holding real keyboard focus"},
    {"keyboard_focused", "bool", "some surface has keyboard focus"},
    {"layer", "str|null", "namespace of focused layer-shell surface"},
    {"seat", "bool", "a seat exists"},
    {"title", "str|null", "title of the focused view"},
};

static const struct ipc_field_doc get_trace_fields[] = {
    {"enabled", "bool", "false unless started with --trace"},
    {"capacity", "int", "ring size"},
    {"count", "int", "entries still in the ring"},
    {"total", "int", "entries ever written"},
    {"dropped", "int", "total - count: overwritten before you asked"},
    {"entries", "obj[]",
     "seq, ms, obj, what, detail — what/why of each "
     "state-change decision, oldest first"},
};

static const struct ipc_field_doc get_listeners_fields[] = {
    {"count", "int", "listeners registered right now"},
    {"overflow", "int",
     "non-zero means registrations the table could not "
     "record: a leak, not a dump problem"},
    {"listeners", "obj[]",
     "seq (registration order — listeners fire in "
     "this order), name, emits"},
};

static const struct ipc_field_doc dump_scene_fields[] = {
    {"tree", "obj",
     "scene graph: type, enabled, x, y, w, h, "
     "rgba|opacity+has_buffer, rect corners+clip, children[], truncated"},
    {"nodes", "int", "nodes emitted (caps at 2000, depth 6)"},
    {"error", "str", "set instead of tree when no scene exists"},
};

static const struct ipc_field_doc self_check_fields[] = {
    {"ok", "bool", "true when every invariant held"},
    {"failures", "int", "number of failed checks"},
    {"checks", "obj[]", "name, ok, and expected/actual on failure"},
};

static const struct ipc_field_doc get_config_fields[] = {
    {"config", "obj", "full runtime configuration tree"},
};

static const struct ipc_field_doc reload_config_fields[] = {
    {"reloaded", "bool", "true if reloaded from config file"},
    {"path", "str|null", "path of loaded configuration file"},
};

static const struct ipc_field_doc help_fields[] = {
    {"commands", "obj[]", "this list: name, desc, fields[{name,type,desc}]"},
};
static const struct ipc_cmd_doc ipc_cmd_docs[] = {
    {"ping", "liveness probe", ping_fields, IPC_FIELDS_LEN(ping_fields)},
    {"get_tree", "full compositor state", get_tree_fields, IPC_FIELDS_LEN(get_tree_fields)},
    {"get_focus", "who has focus and why (recorded vs keyboard)", get_focus_fields_doc,
     IPC_FIELDS_LEN(get_focus_fields_doc)},
    {"get_trace", "recent state-change decisions (needs --trace)", get_trace_fields,
     IPC_FIELDS_LEN(get_trace_fields)},
    {"get_listeners",
     "wl_listener registry: registration order is "
     "call order (see AGENTS.md pitfall 2)",
     get_listeners_fields, IPC_FIELDS_LEN(get_listeners_fields)},
    {"dump_scene", "wlr_scene graph as JSON (for drawn-but-invisible bugs)", dump_scene_fields,
     IPC_FIELDS_LEN(dump_scene_fields)},
    {"self_check", "invariant assertions (alias: self-check)", self_check_fields,
     IPC_FIELDS_LEN(self_check_fields)},
    {"get_config", "runtime configuration as JSON", get_config_fields,
     IPC_FIELDS_LEN(get_config_fields)},
    {"reload_config", "hot-reload configuration from file", reload_config_fields,
     IPC_FIELDS_LEN(reload_config_fields)},
    {"help", "this response", help_fields, IPC_FIELDS_LEN(help_fields)},
};

/*
 * 和 dispatch() 的同步哨兵。dispatch 是手写的 if-else 链，docs 是这张
 * 表，两边没有编译期的绑定——加命令时只改 docs 不改 dispatch（或反之）
 * 不会有任何报错。这行断言把命令数钉死在 IPC_CMD_COUNT：改了 docs 的
 * 条目数而忘了碰计数（或反过来），编译立刻失败，逼着添加命令的人
 * 把 dispatch、docs、计数三处一起过一遍。dispatch 单边新增依然没有
 * 编译期防护——那条靠评审兜底。
 */
#define IPC_CMD_COUNT 10
_Static_assert(sizeof(ipc_cmd_docs) / sizeof(ipc_cmd_docs[0]) == IPC_CMD_COUNT,
               "ipc_cmd_docs changed: update dispatch() and IPC_CMD_COUNT "
               "together (add one, bump the other)");

/* ==================== JSON 拼装小工具 ==================== */

/*
 * 逗号自动管理的对象写入器已经提升为公共设施（util/json.h 的
 * woodland_json_obj_*），这里只剩领域特定的字段助手：
 * 盒子写成 [x,y,w,h]、视图引用写成 id 或 null、贴边写成字符串数组。
 */

/* 盒子：jq 里 .outer[2] 就是宽，比 {x,y,w,h} 好取 */
static void obj_box(struct woodland_json_obj *obj, const char *key, const struct wlr_box *box)
{
    int values[4] = {box->x, box->y, box->width, box->height};
    woodland_json_obj_key(obj, key);
    woodland_json_int_array(obj->json, values, 4);
}

/* 视图引用：写成 id，没有则 null（比指针好读，也能跨进程用） */
static void obj_view_id(struct woodland_json_obj *obj, const char *key,
                        const struct woodland_view *view)
{
    woodland_json_obj_key(obj, key);
    if (view) {
        woodland_json_int(obj->json, view->id);
    } else {
        woodland_json_raw(obj->json, "null");
    }
}

static const char *view_kind(const struct woodland_view *view)
{
    return view->type == WOODLAND_VIEW_XWAYLAND ? "x11" : "xdg";
}

/* 贴边位掩码 → 字符串数组（left/right/top/bottom） */
static void obj_snapped(struct woodland_json_obj *obj, uint32_t edges)
{
    static const struct {
        uint32_t edge;
        const char *name;
    } names[] = {
        {WLR_EDGE_LEFT, "left"},
        {WLR_EDGE_RIGHT, "right"},
        {WLR_EDGE_TOP, "top"},
        {WLR_EDGE_BOTTOM, "bottom"},
    };
    woodland_json_obj_key(obj, "snapped");
    /* arr 和 obj 共享同一个底层 woodland_json：sep 写分隔符、
     * string 写值，各自经自己的游标记状态，落点却是同一处 */
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, obj->json);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(edges & names[i].edge)) {
            continue;
        }
        woodland_json_arr_sep(&arr);
        woodland_json_string(obj->json, names[i].name);
    }
    woodland_json_arr_close(&arr);
}

static const char *view_output_name(struct woodland_view *view)
{
    if (!view->server || !view->server->output_layout) {
        return NULL;
    }
    struct woodland_output *wo = woodland_output_for_view(view);
    if (!wo || !wo->wlr_output) {
        return NULL;
    }
    return wo->wlr_output->name;
}

/* ==================== get_tree ==================== */

static void write_focus_fields(struct woodland_server *server, struct woodland_json_obj *obj);

/* 只写值（数组），"outputs" 这个 key 由调用方负责 */
static void append_outputs(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);

    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link)
    {
        if (!wo->wlr_output) {
            continue;
        }
        woodland_json_arr_sep(&arr);

        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_str(&obj, "name", wo->wlr_output->name);
        woodland_json_obj_bool(&obj, "enabled", wo->wlr_output->enabled);

        if (server->output_layout) {
            struct wlr_box box = {0};
            wlr_output_layout_get_box(server->output_layout, wo->wlr_output, &box);
            obj_box(&obj, "box", &box);
            struct wlr_box area = {0};
            woodland_output_workarea(wo, &area);
            obj_box(&obj, "workarea", &area);
        }
        woodland_json_obj_int(&obj, "width", wo->wlr_output->width);
        woodland_json_obj_int(&obj, "height", wo->wlr_output->height);
        woodland_json_obj_int(&obj, "scale", (int)(wo->wlr_output->scale * 1000.0f));
        woodland_json_obj_close(&obj);
    }
    woodland_json_arr_close(&arr);
}

/* 只写值（对象），"workspace" 这个 key 由调用方负责 */
static void append_workspace(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_str(&obj, "current",
                          server->active_workspace ? server->active_workspace->name : NULL);
    woodland_json_obj_key(&obj, "all");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);

    struct woodland_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link)
    {
        woodland_json_arr_sep(&arr);
        woodland_json_string(out, ws->name);
    }
    woodland_json_arr_close(&arr);
    woodland_json_obj_close(&obj);
}

static void append_view_details(struct woodland_server *server, struct woodland_view *view, int z,
                                struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_int(&obj, "id", view->id);
    woodland_json_obj_str(&obj, "kind", view_kind(view));
    woodland_json_obj_int(&obj, "z", z);
    woodland_json_obj_str(&obj, "title", view->title);
    woodland_json_obj_str(&obj, "app_id", view->app_id);
    /* xdg-toplevel-tag：协议层只记录不使用，靠这里才看得见 */
    woodland_json_obj_str(&obj, "tag", view->tag);
    woodland_json_obj_str(&obj, "workspace", view->workspace ? view->workspace->name : NULL);

    const char *output = view_output_name(view);
    woodland_json_obj_str(&obj, "output", output);

    woodland_json_obj_bool(&obj, "mapped", view->mapped);
    woodland_json_obj_bool(&obj, "placed", view->placed);
    woodland_json_obj_bool(&obj, "minimized", view->minimized);
    woodland_json_obj_bool(&obj, "maximized", view->maximized);
    woodland_json_obj_bool(&obj, "fullscreen", view->fullscreen);
    woodland_json_obj_bool(&obj, "override_redirect", view->override_redirect);
    woodland_json_obj_bool(&obj, "csd", view->deco_csd);
    woodland_json_obj_bool(&obj, "focused", server->focused_view == view);
    woodland_json_obj_bool(&obj, "activated", view->activated);
    woodland_json_obj_bool(&obj, "focusable", woodland_view_focusable(view));
    woodland_json_obj_bool(&obj, "scene_visible",
                           view->scene_tree && view->scene_tree->node.enabled);
    obj_view_id(&obj, "parent", view->parent);

    struct wlr_box outer = {0};
    woodland_view_outer_box(view, &outer);
    obj_box(&obj, "outer", &outer);

    /* 内容矩形换算到布局坐标：调用方不用再关心 scene_tree 原点是内容原点 */
    struct wlr_box content = {0};
    woodland_view_content_box(view, &content);
    if (view->scene_tree) {
        content.x += view->scene_tree->node.x;
        content.y += view->scene_tree->node.y;
    }
    obj_box(&obj, "content", &content);

    woodland_json_obj_key(&obj, "ssd");
    struct woodland_json_obj ssd;
    woodland_json_obj_open(&ssd, out);
    woodland_json_obj_bool(&ssd, "present", view->ssd != NULL);
    woodland_json_obj_bool(&ssd, "active", woodland_ssd_is_active(view));
    struct woodland_ssd_margins margins = {0};
    woodland_ssd_get_margins(view, &margins);
    int margin_values[4] = {margins.left, margins.top, margins.right, margins.bottom};
    woodland_json_obj_key(&ssd, "margins");
    woodland_json_int_array(out, margin_values, 4);
    woodland_json_obj_close(&ssd);

    /*
     * treeland-decoration 的每窗口覆盖。只回"客户端接管了什么"：
     * 没 set 过的属性不出现（协议里每个属性都是 opt-in），这样
     * `jq '.views[0].deco'` 一眼能看出这个窗口被改了什么。
     */
    woodland_json_obj_key(&obj, "deco");
    struct woodland_json_obj deco;
    woodland_json_obj_open(&deco, out);
    woodland_json_obj_bool(&deco, "managed", view->ssd_overrides.managed);
    if (view->ssd_overrides.titlebar_set) {
        woodland_json_obj_str(&deco, "titlebar",
                              view->ssd_overrides.titlebar_hidden ? "hidden" : "shown");
    }
    if (view->ssd_overrides.border_set) {
        int rgba[4] = {(int)(view->ssd_overrides.border_rgba >> 24) & 0xFF,
                       (int)(view->ssd_overrides.border_rgba >> 16) & 0xFF,
                       (int)(view->ssd_overrides.border_rgba >> 8) & 0xFF,
                       (int)view->ssd_overrides.border_rgba & 0xFF};
        woodland_json_obj_int(&deco, "border_width", view->ssd_overrides.border_width);
        woodland_json_obj_key(&deco, "border_rgba");
        woodland_json_int_array(out, rgba, 4);
    }
    if (view->ssd_overrides.corner_radius_set) {
        woodland_json_obj_int(&deco, "corner_radius", view->ssd_overrides.corner_radius);
    }
    if (view->ssd_overrides.shadow_set) {
        int offset[2] = {view->ssd_overrides.shadow_dx, view->ssd_overrides.shadow_dy};
        int rgba[4] = {(int)(view->ssd_overrides.shadow_rgba >> 24) & 0xFF,
                       (int)(view->ssd_overrides.shadow_rgba >> 16) & 0xFF,
                       (int)(view->ssd_overrides.shadow_rgba >> 8) & 0xFF,
                       (int)view->ssd_overrides.shadow_rgba & 0xFF};
        woodland_json_obj_int(&deco, "shadow_radius", view->ssd_overrides.shadow_radius);
        woodland_json_obj_key(&deco, "shadow_offset");
        woodland_json_int_array(out, offset, 2);
        woodland_json_obj_key(&deco, "shadow_rgba");
        woodland_json_int_array(out, rgba, 4);
    }
    /* 真正会改变画面的属性：没接 scenefx 时圆角/阴影只记录，别在断言里当它们生效 */
    woodland_json_obj_key(&deco, "rendered");
    struct woodland_json_arr rendered;
    woodland_json_arr_open(&rendered, out);
    if (view->ssd_overrides.border_set) {
        woodland_json_arr_sep(&rendered);
        woodland_json_string(out, "border");
    }
    if (view->ssd_overrides.titlebar_set) {
        woodland_json_arr_sep(&rendered);
        woodland_json_string(out, "titlebar");
    }
    if (view->ssd_overrides.corner_radius_set &&
        woodland_effect_server_enabled(view->server)) {
        woodland_json_arr_sep(&rendered);
        woodland_json_string(out, "corner_radius");
    }
    woodland_json_arr_close(&rendered);
    woodland_json_obj_close(&deco);

    /* ext-background-effect 的已提交状态：blur 为真才真的有模糊 */
    woodland_json_obj_key(&obj, "bg_effect");
    struct woodland_json_obj bg;
    woodland_json_obj_open(&bg, out);
    woodland_json_obj_bool(&bg, "blur", view->effect.blur);
    int blur_box[4] = {view->effect.blur_region.x, view->effect.blur_region.y,
                       view->effect.blur_region.width, view->effect.blur_region.height};
    woodland_json_obj_key(&bg, "region");
    woodland_json_int_array(out, blur_box, 4);
    woodland_json_obj_close(&bg);

    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    woodland_view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    int limits[4] = {min_w, min_h, max_w, max_h};
    woodland_json_obj_key(&obj, "limits");
    woodland_json_int_array(out, limits, 4);

    obj_snapped(&obj, view->snapped_edges);
    woodland_json_obj_close(&obj);
}

static void ipc_get_tree(struct woodland_server *server, struct woodland_json *out)
{
    /* first=false：ok/cmd 前缀已经手写，后面的字段自动带前置逗号 */
    struct woodland_json_obj obj = {.json = out, .first = false};
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"get_tree\"");
    woodland_json_obj_int(&obj, "pid", (long long)getpid());
    woodland_json_obj_str(&obj, "ipc", server->ipc_path);
    woodland_json_obj_str(&obj, "placement_policy",
                          woodland_placement_policy_name(server->placement_policy));
    woodland_json_obj_bool(&obj, "self_check", server->self_check);
    /* 这台合成器此刻会不会画圆角/模糊：脚本据此决定 deco.rendered 的期望值。
     * 编译期没有开关了，这里是 [effects] enabled 的实时值。 */
    woodland_json_obj_bool(&obj, "effects", woodland_effect_server_enabled(server));
    woodland_json_obj_bool(&obj, "effects_blur",
                           woodland_effect_server_blur_enabled(server));

    woodland_json_obj_key(&obj, "outputs");
    append_outputs(server, out);
    woodland_json_obj_key(&obj, "workspace");
    append_workspace(server, out);

    woodland_json_obj_key(&obj, "focus");
    struct woodland_json_obj focus_obj;
    woodland_json_obj_open(&focus_obj, out);
    write_focus_fields(server, &focus_obj);
    woodland_json_obj_close(&focus_obj);

    woodland_json_obj_key(&obj, "views");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);
    int count = 0;
    int z = 0;
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link)
    {
        woodland_json_arr_sep(&arr);
        count++;
        append_view_details(server, view, z++, out);
    }
    woodland_json_arr_close(&arr);
    woodland_json_obj_int(&obj, "count", count);
    woodland_json_obj_close(&obj);
}

/* ==================== get_focus ==================== */

/*
 * 焦点字段。抽成"往已有对象里写字段"的形式，get_tree 里嵌成
 * "focus": {...}，get_focus 则直接摊在顶层——两边永远说同一套字段。
 */
static void write_focus_fields(struct woodland_server *server, struct woodland_json_obj *obj)
{
    struct woodland_view *focused = woodland_focus_current(server);
    struct woodland_view *keyboard_view = NULL;
    if (server->seat) {
        keyboard_view = woodland_view_from_surface(server->seat->keyboard_state.focused_surface);
    }

    obj_view_id(obj, "view", focused);
    obj_view_id(obj, "recorded_view", server->focused_view);
    obj_view_id(obj, "keyboard_view", keyboard_view);
    woodland_json_obj_bool(obj, "keyboard_focused",
                           server->seat && server->seat->keyboard_state.focused_surface);
    woodland_json_obj_str(obj, "layer",
                          server->focused_layer ? server->focused_layer->namespace : NULL);
    woodland_json_obj_bool(obj, "seat", server->seat != NULL);
    woodland_json_obj_str(obj, "title", focused ? focused->title : NULL);
}

static void ipc_get_focus(struct woodland_server *server, struct woodland_json *out)
{
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"get_focus\"");
    /* first=false：沿用已经写了一半的顶层对象，字段会带前置逗号 */
    struct woodland_json_obj obj = {.json = out, .first = false};
    write_focus_fields(server, &obj);
    woodland_json_raw(out, "}");
}

/* ==================== dump_scene ==================== */

struct scene_dump_ctx {
    struct woodland_json *out;
    int nodes;
    int max_nodes;
    int max_depth;
};

static void dump_node(struct scene_dump_ctx *ctx, struct wlr_scene_node *node, int depth)
{
    struct woodland_json *out = ctx->out;
    if (!node || ctx->nodes >= ctx->max_nodes) {
        return;
    }
    ctx->nodes++;

    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    switch (node->type) {
    case WLR_SCENE_NODE_TREE:
        woodland_json_obj_str(&obj, "type", "tree");
        break;
    case WLR_SCENE_NODE_RECT:
        woodland_json_obj_str(&obj, "type", "rect");
        break;
    case WLR_SCENE_NODE_BUFFER:
        woodland_json_obj_str(&obj, "type", "buffer");
        break;
    /*
     * 模糊节点只在客户端声明了 blur 时才存在。dump_scene 是排查"画了但看不见"
     * 的第一现场，所以宁可把 blur 节点的尺寸/四角也吐出来，也不要让它掉进
     * unknown——那样看起来像节点根本没建。
     */
    case WLR_SCENE_NODE_BLUR:
        woodland_json_obj_str(&obj, "type", "blur");
        break;
    case WLR_SCENE_NODE_OPTIMIZED_BLUR:
        woodland_json_obj_str(&obj, "type", "optimized_blur");
        break;
    default:
        woodland_json_obj_str(&obj, "type", "unknown");
        break;
    }
    woodland_json_obj_bool(&obj, "enabled", node->enabled);
    woodland_json_obj_int(&obj, "x", node->x);
    woodland_json_obj_int(&obj, "y", node->y);

    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        woodland_json_obj_int(&obj, "w", rect->width);
        woodland_json_obj_int(&obj, "h", rect->height);
        /* 颜色按 0-255 给，和人眼/截图工具对得上（内部是 linear float） */
        int rgba[4];
        for (int i = 0; i < 4; i++) {
            float c = rect->color[i];
            rgba[i] = (int)(c * 255.0f + 0.5f);
        }
        woodland_json_obj_key(&obj, "rgba");
        woodland_json_int_array(out, rgba, 4);
        /*
         * 圆角描边的两半：外圆角 + 挖空内框。边框"看着还是直角"时先看这里
         * ——只在 1px 细条上设 corners 会被尺寸 clamp 掉，必须有 clip 才是描边。
         */
        int corners[4] = {rect->corners.top_left, rect->corners.top_right,
                          rect->corners.bottom_right, rect->corners.bottom_left};
        woodland_json_obj_key(&obj, "corners");
        woodland_json_int_array(out, corners, 4);
        woodland_json_obj_key(&obj, "clip");
        struct woodland_json_obj clip;
        woodland_json_obj_open(&clip, out);
        woodland_json_obj_int(&clip, "x", rect->clipped_region.area.x);
        woodland_json_obj_int(&clip, "y", rect->clipped_region.area.y);
        woodland_json_obj_int(&clip, "w", rect->clipped_region.area.width);
        woodland_json_obj_int(&clip, "h", rect->clipped_region.area.height);
        int clip_corners[4] = {
            rect->clipped_region.corners.top_left, rect->clipped_region.corners.top_right,
            rect->clipped_region.corners.bottom_right, rect->clipped_region.corners.bottom_left};
        woodland_json_obj_key(&clip, "corners");
        woodland_json_int_array(out, clip_corners, 4);
        woodland_json_obj_close(&clip);
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        woodland_json_obj_int(&obj, "w", buffer->dst_width);
        woodland_json_obj_int(&obj, "h", buffer->dst_height);
        woodland_json_obj_int(&obj, "opacity", (int)(buffer->opacity * 100.0f + 0.5f));
        woodland_json_obj_bool(&obj, "has_buffer", buffer->buffer != NULL);
    } else if (node->type == WLR_SCENE_NODE_BLUR) {
        struct wlr_scene_blur *blur = wlr_scene_blur_from_node(node);
        woodland_json_obj_int(&obj, "w", blur->width);
        woodland_json_obj_int(&obj, "h", blur->height);
        int corners[4] = {blur->corners.top_left, blur->corners.top_right,
                          blur->corners.bottom_right, blur->corners.bottom_left};
        woodland_json_obj_key(&obj, "corners");
        woodland_json_int_array(out, corners, 4);
    }

    if (node->type == WLR_SCENE_NODE_TREE && depth < ctx->max_depth) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        woodland_json_obj_key(&obj, "children");
        struct woodland_json_arr arr;
        woodland_json_arr_open(&arr, out);
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link)
        {
            if (ctx->nodes >= ctx->max_nodes) {
                break;
            }
            woodland_json_arr_sep(&arr);
            dump_node(ctx, child, depth + 1);
        }
        woodland_json_arr_close(&arr);
        if (ctx->nodes >= ctx->max_nodes) {
            woodland_json_obj_bool(&obj, "truncated", true);
        }
    }
    woodland_json_obj_close(&obj);
}

static void ipc_dump_scene(struct woodland_server *server, struct woodland_json *out)
{
    /* first=false：ok/cmd 前缀已手写 */
    struct woodland_json_obj obj = {.json = out, .first = false};
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"dump_scene\"");
    if (!server->scene) {
        woodland_json_obj_str(&obj, "error", "no scene");
        woodland_json_obj_close(&obj);
        return;
    }
    woodland_json_obj_key(&obj, "tree");
    struct scene_dump_ctx ctx = {
        .out = out,
        .nodes = 0,
        .max_nodes = SCENE_MAX_NODES,
        .max_depth = SCENE_MAX_DEPTH,
    };
    dump_node(&ctx, &server->scene->tree.node, 0);
    woodland_json_obj_int(&obj, "nodes", ctx.nodes);
    woodland_json_obj_close(&obj);
}

static void ipc_get_config(struct woodland_server *server, struct woodland_json *out)
{
    struct woodland_json_obj obj = {.json = out, .first = false};
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"get_config\"");
    woodland_json_obj_key(&obj, "config");
    if (server->config) {
        woodland_config_dump_json(server->config, out);
    } else {
        woodland_json_raw(out, "null");
    }
    woodland_json_obj_close(&obj);
}

static bool ipc_reload_config(struct woodland_server *server, struct woodland_json *out)
{
    bool ok = woodland_config_reload(server);
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", ok);
    woodland_json_obj_str(&obj, "cmd", "reload_config");
    woodland_json_obj_bool(&obj, "reloaded", ok);
    woodland_json_obj_str(&obj, "path", server->config ? server->config->loaded_path : NULL);
    woodland_json_obj_close(&obj);
    return ok;
}

/* ==================== 分发 ==================== */

/* help 的输出从 ipc_cmd_docs 生成：表是唯一事实源，不会和实现漂移 */
static void ipc_help(struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", true);
    woodland_json_obj_str(&obj, "cmd", "help");

    woodland_json_obj_key(&obj, "commands");
    struct woodland_json_arr cmds;
    woodland_json_arr_open(&cmds, out);
    for (size_t i = 0; i < sizeof(ipc_cmd_docs) / sizeof(ipc_cmd_docs[0]); i++) {
        const struct ipc_cmd_doc *doc = &ipc_cmd_docs[i];
        woodland_json_arr_sep(&cmds);

        struct woodland_json_obj cmd_obj;
        woodland_json_obj_open(&cmd_obj, out);
        woodland_json_obj_str(&cmd_obj, "name", doc->name);
        woodland_json_obj_str(&cmd_obj, "desc", doc->desc);

        woodland_json_obj_key(&cmd_obj, "fields");
        struct woodland_json_arr fields;
        woodland_json_arr_open(&fields, out);
        for (size_t f = 0; f < doc->nfields; f++) {
            woodland_json_arr_sep(&fields);
            struct woodland_json_obj field;
            woodland_json_obj_open(&field, out);
            woodland_json_obj_str(&field, "name", doc->fields[f].name);
            woodland_json_obj_str(&field, "type", doc->fields[f].type);
            woodland_json_obj_str(&field, "desc", doc->fields[f].desc);
            woodland_json_obj_close(&field);
        }
        woodland_json_arr_close(&fields);
        woodland_json_obj_close(&cmd_obj);
    }
    woodland_json_arr_close(&cmds);
    woodland_json_obj_close(&obj);
}

static void respond_error(struct woodland_json *out, const char *cmd, const char *message)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);
    woodland_json_obj_bool(&obj, "ok", false);
    woodland_json_obj_str(&obj, "cmd", cmd);
    woodland_json_obj_str(&obj, "error", message);
    woodland_json_obj_close(&obj);
}

static bool dispatch(struct woodland_server *server, const char *line, struct woodland_json *out)
{
    /* 取第一个空白分隔的词作为命令名 */
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char cmd[32];
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && n + 1 < sizeof(cmd)) {
        cmd[n] = p[n];
        n++;
    }
    cmd[n] = '\0';

    if (cmd[0] == '\0') {
        respond_error(out, "", "empty command");
        return false;
    }
    if (strcmp(cmd, "help") == 0) {
        ipc_help(out);
        return true;
    }
    if (strcmp(cmd, "ping") == 0) {
        struct woodland_json_obj obj;
        woodland_json_obj_open(&obj, out);
        woodland_json_obj_bool(&obj, "ok", true);
        woodland_json_obj_str(&obj, "cmd", "ping");
        woodland_json_obj_int(&obj, "pid", (long long)getpid());
        woodland_json_obj_close(&obj);
        return true;
    }
    if (strcmp(cmd, "get_tree") == 0) {
        ipc_get_tree(server, out);
        return true;
    }
    if (strcmp(cmd, "get_focus") == 0) {
        ipc_get_focus(server, out);
        return true;
    }
    if (strcmp(cmd, "get_trace") == 0) {
        /* 未开 --trace 时也回 200：是一份 enabled=false 的完整响应，
         * 脚本不用为"有没有开"分叉 */
        woodland_trace_dump(out);
        return true;
    }
    if (strcmp(cmd, "get_listeners") == 0) {
        /* 注册序号即调用顺序：一眼看出漏注册/顺序反了 */
        woodland_listener_dump(out);
        return true;
    }
    if (strcmp(cmd, "dump_scene") == 0) {
        ipc_dump_scene(server, out);
        return true;
    }
    if (strcmp(cmd, "self_check") == 0 || strcmp(cmd, "self-check") == 0) {
        /* selfcheck_run 用 first=false 接着这个写了一半的对象继续 */
        woodland_json_raw(out, "{\"cmd\":\"self_check\"");
        int failures = woodland_selfcheck_run(server, out);
        woodland_json_raw(out, "}");
        return failures == 0;
    }
    if (strcmp(cmd, "get_config") == 0 || strcmp(cmd, "get-config") == 0 ||
        strcmp(cmd, "config") == 0) {
        ipc_get_config(server, out);
        return true;
    }
    if (strcmp(cmd, "reload_config") == 0 || strcmp(cmd, "reload-config") == 0 ||
        strcmp(cmd, "reload") == 0) {
        return ipc_reload_config(server, out);
    }

    respond_error(out, cmd, "unknown command, try 'help'");
    return false;
}

/*
 * 对外契约比内部实现多一条：**只要 out 交了回来，里面一定是一段完整的
 * JSON**。拼装中途 OOM 时，缓冲区里是半截对象，传输层拿它当响应发出去
 * 会让客户端解析失败；把它在这里换成一整条错误响应，调用方就不用知道
 * 内存失败这回事——那本来就是查询层的问题。
 */
bool woodland_ipc_dispatch(struct woodland_server *server, const char *line,
                           struct woodland_json *out)
{
    if (!server || !line || !out) {
        return false;
    }
    bool ok = dispatch(server, line, out);
    if (!woodland_json_ok(out)) {
        woodland_json_finish(out);
        woodland_json_init(out);
        respond_error(out, "", "out of memory");
        return false;
    }
    return ok;
}
