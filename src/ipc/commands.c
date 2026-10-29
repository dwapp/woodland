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
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>

#include "focus.h"
#include "ipc.h"
#include "output.h"
#include "placement.h"
#include "selfcheck.h"
#include "ssd.h"
#include "util/json.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

/* 场景树转储的递归深度与节点上限：防止一次 dump 输出几百 KB */
#define SCENE_MAX_DEPTH 6
#define SCENE_MAX_NODES 2000

#define IPC_COMMANDS "ping, get_tree, get_focus, dump_scene, self_check, help"

/* ==================== JSON 拼装小工具 ==================== */

/*
 * 对象写入器：自动处理分隔逗号。
 *
 * 手写 JSON 最容易错的就是逗号（首字段多一个、末字段少一个），
 * 这里把"是否首字段"记在结构里，调用方只关心键值。
 */
struct json_obj {
    struct woodland_json *json;
    bool first;
};

static void obj_open(struct json_obj *obj, struct woodland_json *json) {
    obj->json = json;
    obj->first = true;
    woodland_json_raw(json, "{");
}

static void obj_sep(struct json_obj *obj) {
    if (!obj->first) {
        woodland_json_raw(obj->json, ",");
    }
    obj->first = false;
}

static void obj_key(struct json_obj *obj, const char *key) {
    obj_sep(obj);
    woodland_json_key(obj->json, key);
}

static void obj_int(struct json_obj *obj, const char *key, long long value) {
    obj_key(obj, key);
    woodland_json_int(obj->json, value);
}

static void obj_bool(struct json_obj *obj, const char *key, bool value) {
    obj_key(obj, key);
    woodland_json_bool(obj->json, value);
}

static void obj_str(struct json_obj *obj, const char *key, const char *value) {
    obj_key(obj, key);
    woodland_json_string(obj->json, value);
}

static void obj_box(struct json_obj *obj, const char *key,
                    const struct wlr_box *box) {
    int values[4] = {box->x, box->y, box->width, box->height};
    obj_key(obj, key);
    woodland_json_int_array(obj->json, values, 4);
}

/* 视图引用：写成 id，没有则 null（比指针好读，也能跨进程用） */
static void obj_view_id(struct json_obj *obj, const char *key,
                        const struct woodland_view *view) {
    obj_key(obj, key);
    if (view) {
        woodland_json_int(obj->json, view->id);
    } else {
        woodland_json_raw(obj->json, "null");
    }
}

static void obj_close(struct json_obj *obj) {
    woodland_json_raw(obj->json, "}");
}

static const char *view_kind(const struct woodland_view *view) {
    return view->type == WOODLAND_VIEW_XWAYLAND ? "x11" : "xdg";
}

/* 贴边位掩码 → 字符串数组（left/right/top/bottom） */
static void obj_snapped(struct json_obj *obj, uint32_t edges) {
    static const struct {
        uint32_t edge;
        const char *name;
    } names[] = {
        {WLR_EDGE_LEFT, "left"},
        {WLR_EDGE_RIGHT, "right"},
        {WLR_EDGE_TOP, "top"},
        {WLR_EDGE_BOTTOM, "bottom"},
    };
    obj_key(obj, "snapped");
    woodland_json_raw(obj->json, "[");
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(edges & names[i].edge)) {
            continue;
        }
        if (!first) {
            woodland_json_raw(obj->json, ",");
        }
        first = false;
        woodland_json_string(obj->json, names[i].name);
    }
    woodland_json_raw(obj->json, "]");
}

static const char *view_output_name(struct woodland_view *view) {
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

static void write_focus_fields(struct woodland_server *server,
                               struct json_obj *obj);

static void append_outputs(struct woodland_server *server,
                           struct woodland_json *out) {
    woodland_json_key(out, "outputs");
    woodland_json_raw(out, "[");
    bool first = true;

    struct woodland_output *wo;
    wl_list_for_each(wo, &server->outputs, link) {
        if (!wo->wlr_output) {
            continue;
        }
        if (!first) {
            woodland_json_raw(out, ",");
        }
        first = false;

        struct json_obj obj;
        obj_open(&obj, out);
        obj_str(&obj, "name", wo->wlr_output->name);
        obj_bool(&obj, "enabled", wo->wlr_output->enabled);

        if (server->output_layout) {
            struct wlr_box box = {0};
            wlr_output_layout_get_box(server->output_layout, wo->wlr_output,
                                      &box);
            obj_box(&obj, "box", &box);
            struct wlr_box area = {0};
            woodland_output_workarea(wo, &area);
            obj_box(&obj, "workarea", &area);
        }
        obj_int(&obj, "width", wo->wlr_output->width);
        obj_int(&obj, "height", wo->wlr_output->height);
        obj_int(&obj, "scale", (int)(wo->wlr_output->scale * 1000.0f));
        obj_close(&obj);
    }
    woodland_json_raw(out, "]");
}

static void append_workspace(struct woodland_server *server,
                             struct woodland_json *out) {
    woodland_json_key(out, "workspace");
    woodland_json_raw(out, "{");
    woodland_json_key(out, "current");
    woodland_json_string(out, server->active_workspace
                                   ? server->active_workspace->name
                                   : NULL);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "all");
    woodland_json_raw(out, "[");

    bool first = true;
    struct woodland_workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (!first) {
            woodland_json_raw(out, ",");
        }
        first = false;
        woodland_json_string(out, ws->name);
    }
    woodland_json_raw(out, "]}");
}

static void append_view_details(struct woodland_server *server,
                                struct woodland_view *view, int z,
                                struct woodland_json *out) {
    struct json_obj obj;
    obj_open(&obj, out);
    obj_int(&obj, "id", view->id);
    obj_str(&obj, "kind", view_kind(view));
    obj_int(&obj, "z", z);
    obj_str(&obj, "title", view->title);
    obj_str(&obj, "app_id", view->app_id);
    /* xdg-toplevel-tag：协议层只记录不使用，靠这里才看得见 */
    obj_str(&obj, "tag", view->tag);
    obj_str(&obj, "workspace", view->workspace ? view->workspace->name : NULL);

    const char *output = view_output_name(view);
    obj_str(&obj, "output", output);

    obj_bool(&obj, "mapped", view->mapped);
    obj_bool(&obj, "placed", view->placed);
    obj_bool(&obj, "minimized", view->minimized);
    obj_bool(&obj, "maximized", view->maximized);
    obj_bool(&obj, "fullscreen", view->fullscreen);
    obj_bool(&obj, "override_redirect", view->override_redirect);
    obj_bool(&obj, "csd", view->deco_csd);
    obj_bool(&obj, "focused", server->focused_view == view);
    obj_bool(&obj, "activated", view->activated);
    obj_bool(&obj, "focusable", woodland_view_focusable(view));
    obj_bool(&obj, "scene_visible", view->scene_tree
                                    && view->scene_tree->node.enabled);
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

    obj_key(&obj, "ssd");
    woodland_json_raw(out, "{");
    woodland_json_key(out, "present");
    woodland_json_bool(out, view->ssd != NULL);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "active");
    woodland_json_bool(out, woodland_ssd_is_active(view));
    struct woodland_ssd_margins margins = {0};
    woodland_ssd_get_margins(view, &margins);
    int margin_values[4] = {margins.left, margins.top, margins.right,
                            margins.bottom};
    woodland_json_raw(out, ",");
    woodland_json_key(out, "margins");
    woodland_json_int_array(out, margin_values, 4);
    woodland_json_raw(out, "}");

    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    woodland_view_size_limits(view, &min_w, &min_h, &max_w, &max_h);
    int limits[4] = {min_w, min_h, max_w, max_h};
    obj_key(&obj, "limits");
    woodland_json_int_array(out, limits, 4);

    obj_snapped(&obj, view->snapped_edges);
    obj_close(&obj);
}

static void ipc_get_tree(struct woodland_server *server,
                         struct woodland_json *out) {
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"get_tree\",");
    woodland_json_key(out, "pid");
    woodland_json_int(out, (long long)getpid());
    woodland_json_raw(out, ",");
    woodland_json_key(out, "ipc");
    woodland_json_string(out, server->ipc_path);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "placement_policy");
    woodland_json_string(out,
                         woodland_placement_policy_name(server->placement_policy));
    woodland_json_raw(out, ",");
    woodland_json_key(out, "self_check");
    woodland_json_bool(out, server->self_check);
    woodland_json_raw(out, ",");

    append_outputs(server, out);
    woodland_json_raw(out, ",");
    append_workspace(server, out);
    woodland_json_raw(out, ",");

    woodland_json_key(out, "focus");
    woodland_json_raw(out, "{");
    struct json_obj focus_obj = {.json = out, .first = true};
    write_focus_fields(server, &focus_obj);
    woodland_json_raw(out, "},");

    woodland_json_key(out, "views");
    woodland_json_raw(out, "[");
    int count = 0;
    int z = 0;
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (count++ > 0) {
            woodland_json_raw(out, ",");
        }
        append_view_details(server, view, z++, out);
    }
    woodland_json_raw(out, "],");
    woodland_json_key(out, "count");
    woodland_json_int(out, count);
    woodland_json_raw(out, "}");
}

/* ==================== get_focus ==================== */

/*
 * 焦点字段。抽成"往已有对象里写字段"的形式，get_tree 里嵌成
 * "focus": {...}，get_focus 则直接摊在顶层——两边永远说同一套字段。
 */
static void write_focus_fields(struct woodland_server *server,
                               struct json_obj *obj) {
    struct woodland_view *focused = woodland_focus_current(server);
    struct woodland_view *keyboard_view = NULL;
    if (server->seat) {
        keyboard_view = woodland_view_from_surface(
            server->seat->keyboard_state.focused_surface);
    }

    obj_view_id(obj, "view", focused);
    obj_view_id(obj, "recorded_view", server->focused_view);
    obj_view_id(obj, "keyboard_view", keyboard_view);
    obj_bool(obj, "keyboard_focused",
             server->seat && server->seat->keyboard_state.focused_surface);
    obj_str(obj, "layer", server->focused_layer
                             ? server->focused_layer->namespace
                             : NULL);
    obj_bool(obj, "seat", server->seat != NULL);
    obj_str(obj, "title", focused ? focused->title : NULL);
}

static void ipc_get_focus(struct woodland_server *server,
                          struct woodland_json *out) {
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"get_focus\"");
    /* first=false：沿用已经写了一半的顶层对象，字段会带前置逗号 */
    struct json_obj obj = {.json = out, .first = false};
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

static void dump_node(struct scene_dump_ctx *ctx, struct wlr_scene_node *node,
                      int depth) {
    struct woodland_json *out = ctx->out;
    if (!node || ctx->nodes >= ctx->max_nodes) {
        return;
    }
    ctx->nodes++;

    struct json_obj obj;
    obj_open(&obj, out);
    switch (node->type) {
    case WLR_SCENE_NODE_TREE:
        obj_str(&obj, "type", "tree");
        break;
    case WLR_SCENE_NODE_RECT:
        obj_str(&obj, "type", "rect");
        break;
    case WLR_SCENE_NODE_BUFFER:
        obj_str(&obj, "type", "buffer");
        break;
    default:
        obj_str(&obj, "type", "unknown");
        break;
    }
    obj_bool(&obj, "enabled", node->enabled);
    obj_int(&obj, "x", node->x);
    obj_int(&obj, "y", node->y);

    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        obj_int(&obj, "w", rect->width);
        obj_int(&obj, "h", rect->height);
        /* 颜色按 0-255 给，和人眼/截图工具对得上（内部是 linear float） */
        int rgba[4];
        for (int i = 0; i < 4; i++) {
            float c = rect->color[i];
            rgba[i] = (int)(c * 255.0f + 0.5f);
        }
        obj_key(&obj, "rgba");
        woodland_json_int_array(out, rgba, 4);
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        obj_int(&obj, "w", buffer->dst_width);
        obj_int(&obj, "h", buffer->dst_height);
        obj_int(&obj, "opacity", (int)(buffer->opacity * 100.0f + 0.5f));
        obj_bool(&obj, "has_buffer", buffer->buffer != NULL);
    }

    if (node->type == WLR_SCENE_NODE_TREE && depth < ctx->max_depth) {
        struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
        obj_key(&obj, "children");
        woodland_json_raw(out, "[");
        bool first = true;
        struct wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link) {
            if (ctx->nodes >= ctx->max_nodes) {
                break;
            }
            if (!first) {
                woodland_json_raw(out, ",");
            }
            first = false;
            dump_node(ctx, child, depth + 1);
        }
        woodland_json_raw(out, "]");
        if (ctx->nodes >= ctx->max_nodes) {
            obj_bool(&obj, "truncated", true);
        }
    }
    obj_close(&obj);
}

static void ipc_dump_scene(struct woodland_server *server,
                           struct woodland_json *out) {
    woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"dump_scene\"");
    if (!server->scene) {
        woodland_json_raw(out, ",\"error\":\"no scene\"}");
        return;
    }
    woodland_json_raw(out, ",");
    woodland_json_key(out, "tree");
    struct scene_dump_ctx ctx = {
        .out = out,
        .nodes = 0,
        .max_nodes = SCENE_MAX_NODES,
        .max_depth = SCENE_MAX_DEPTH,
    };
    dump_node(&ctx, &server->scene->tree.node, 0);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "nodes");
    woodland_json_int(out, ctx.nodes);
    woodland_json_raw(out, "}");
}

/* ==================== 分发 ==================== */

static void respond_error(struct woodland_json *out, const char *cmd,
                          const char *message) {
    woodland_json_raw(out, "{\"ok\":false,\"cmd\":");
    woodland_json_string(out, cmd);
    woodland_json_raw(out, ",\"error\":");
    woodland_json_string(out, message);
    woodland_json_raw(out, "}");
}

const char *woodland_ipc_commands(void) {
    return IPC_COMMANDS;
}

static bool dispatch(struct woodland_server *server, const char *line,
                     struct woodland_json *out) {
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
        woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"help\",\"commands\":");
        woodland_json_string(out, IPC_COMMANDS);
        woodland_json_raw(out, "}");
        return true;
    }
    if (strcmp(cmd, "ping") == 0) {
        woodland_json_raw(out, "{\"ok\":true,\"cmd\":\"ping\",\"pid\":");
        woodland_json_int(out, (long long)getpid());
        woodland_json_raw(out, "}");
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
    if (strcmp(cmd, "dump_scene") == 0) {
        ipc_dump_scene(server, out);
        return true;
    }
    if (strcmp(cmd, "self_check") == 0 || strcmp(cmd, "self-check") == 0) {
        woodland_json_raw(out, "{\"cmd\":\"self_check\",");
        int failures = woodland_selfcheck_run(server, out);
        woodland_json_raw(out, "}");
        return failures == 0;
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
                           struct woodland_json *out) {
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
