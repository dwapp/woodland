// SPDX-License-Identifier: GPL-2.0-only
/*
 * 调试接口：unix socket + 行分隔 JSON。
 *
 * 只做只读查询（get_tree / get_focus / dump_scene / self_check）。
 * 不给"设置几何""注入按键"这类写命令：写命令需要和正常输入路径抢状态，
 * 一个 bug 就变成"合成器被调试工具搞坏了"，收益不值这个风险。
 *
 * 实现上刻意"笨"：
 *   - 一行命令一行回应，没有长度头，`nc -U` 就能手敲；
 *   - 响应直接拼字符串（util/json.h），失败就回一行错误，不做流控；
 *   - 客户端 fd 用阻塞写：单条响应 < 64KB，远小于 socket 缓冲，
 *     写不动的极端情况（对端不读）由 EPIPE 兜底断开。
 */
#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

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

/* 单行命令上限：正常命令几十字节，留足余量给"手抖粘贴一堆东西" */
#define IPC_MAX_LINE 8192
/* 场景树转储的递归深度与节点上限：防止一次 dump 输出几百 KB */
#define SCENE_MAX_DEPTH 6
#define SCENE_MAX_NODES 2000

#define IPC_COMMANDS "ping, get_tree, get_focus, dump_scene, self_check, help"

struct woodland_ipc_client {
    struct wl_list link;
    struct woodland_server *server;
    int fd;
    struct wl_event_source *source;
    size_t len;
    char buf[IPC_MAX_LINE];
};

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
        int saved = ctx->nodes;
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
        (void)saved;
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

bool woodland_ipc_dispatch(struct woodland_server *server, const char *line,
                           struct woodland_json *out) {
    if (!server || !line || !out) {
        return false;
    }

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

/* ==================== socket 服务端 ==================== */

static bool write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static void client_destroy(struct woodland_ipc_client *client) {
    if (!client) {
        return;
    }
    if (client->source) {
        wl_event_source_remove(client->source);
        client->source = NULL;
    }
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
    wl_list_remove(&client->link);
    free(client);
}

/* 处理一行命令，返回 false 表示连接不可用需要断开 */
static bool client_handle_line(struct woodland_ipc_client *client, char *line) {
    /* 去掉尾部空白（含 telnet/nc 带来的 \r） */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '
                       || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    char *start = line;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start == '\0') {
        return true;   /* 空行当心跳，不回应 */
    }

    struct woodland_json out;
    woodland_json_init(&out);
    woodland_ipc_dispatch(client->server, start, &out);
    if (!woodland_json_ok(&out)) {
        woodland_json_finish(&out);
        woodland_json_init(&out);
        respond_error(&out, "", "out of memory");
    }
    woodland_json_raw(&out, "\n");
    bool ok = write_all(client->fd, woodland_json_data(&out),
                        woodland_json_len(&out));
    woodland_json_finish(&out);
    return ok;
}

static int client_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct woodland_ipc_client *client = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        client_destroy(client);
        return 0;
    }
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    ssize_t n = read(client->fd, client->buf + client->len,
                     sizeof(client->buf) - client->len - 1);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            return 0;
        }
        client_destroy(client);
        return 0;
    }
    client->len += (size_t)n;
    client->buf[client->len] = '\0';

    size_t consumed = 0;
    while (consumed < client->len) {
        char *nl = memchr(client->buf + consumed, '\n',
                          client->len - consumed);
        if (!nl) {
            break;
        }
        *nl = '\0';
        size_t line_off = consumed;
        consumed = (size_t)(nl - client->buf) + 1;
        if (!client_handle_line(client, client->buf + line_off)) {
            client_destroy(client);
            return 0;
        }
    }

    if (consumed > 0) {
        memmove(client->buf, client->buf + consumed, client->len - consumed);
        client->len -= consumed;
        client->buf[client->len] = '\0';
    }

    if (client->len >= sizeof(client->buf) - 1) {
        /* 一直没等到换行：丢掉这一行，别让半条命令无限攒着 */
        static const char too_long[] =
            "{\"ok\":false,\"error\":\"line too long\"}\n";
        if (!write_all(client->fd, too_long, sizeof(too_long) - 1)) {
            client_destroy(client);
            return 0;
        }
        client->len = 0;
        client->buf[0] = '\0';
    }
    return 0;
}

static int ipc_accept(int fd, uint32_t mask, void *data) {
    struct woodland_server *server = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }
    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            wlr_log(WLR_ERROR, "ipc accept failed: %s", strerror(errno));
        }
        return 0;
    }

    struct woodland_ipc_client *client = calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return 0;
    }
    client->server = server;
    client->fd = client_fd;
    client->source = wl_event_loop_add_fd(server->event_loop, client_fd,
                                         WL_EVENT_READABLE, client_readable,
                                         client);
    if (!client->source) {
        close(client_fd);
        free(client);
        return 0;
    }
    wl_list_insert(&server->ipc_clients, &client->link);
    return 0;
}

const char *woodland_ipc_path(const struct woodland_server *server) {
    return (server && server->ipc_path) ? server->ipc_path : "";
}

bool woodland_ipc_init(struct woodland_server *server,
                       const char *wayland_socket) {
    if (!server || !server->event_loop) {
        return false;
    }
    wl_list_init(&server->ipc_clients);

    /* 与 wayland socket 同目录同名规则：一个合成器一个调试 socket，
     * 多实例（嵌套跑测试）不会互相踩 */
    char path[108];   /* sizeof(sockaddr_un.sun_path) */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int written;
    if (runtime && runtime[0]) {
        written = snprintf(path, sizeof(path), "%s/woodland-%s.sock", runtime,
                           wayland_socket ? wayland_socket : "wayland");
    } else {
        written = snprintf(path, sizeof(path), "/tmp/woodland-%d.sock",
                           (int)getpid());
    }
    if (written < 0 || (size_t)written >= sizeof(path)) {
        wlr_log(WLR_ERROR, "ipc socket path too long");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "ipc socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, (size_t)written + 1);

    /* 上一轮崩溃留下的 socket 文件会挡住 bind */
    unlink(path);
    /* 只允许本用户连接：调试接口能读到全部窗口标题 */
    mode_t old_umask = umask(0077);
    bool bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    umask(old_umask);
    if (!bound) {
        wlr_log(WLR_ERROR, "ipc bind %s failed: %s", path, strerror(errno));
        close(fd);
        return false;
    }
    if (listen(fd, 8) < 0) {
        wlr_log(WLR_ERROR, "ipc listen failed: %s", strerror(errno));
        close(fd);
        unlink(path);
        return false;
    }

    server->ipc_fd = fd;
    server->ipc_path = strdup(path);
    server->ipc_source = wl_event_loop_add_fd(server->event_loop, fd,
                                             WL_EVENT_READABLE, ipc_accept,
                                             server);
    if (!server->ipc_source) {
        close(fd);
        server->ipc_fd = -1;
        free(server->ipc_path);
        server->ipc_path = NULL;
        unlink(path);
        return false;
    }

    setenv("WOODLAND_IPC", path, true);
    wlr_log(WLR_INFO, "debug IPC listening on %s (%s)", path, IPC_COMMANDS);
    return true;
}

void woodland_ipc_finish(struct woodland_server *server) {
    if (!server) {
        return;
    }
    struct woodland_ipc_client *client;
    struct woodland_ipc_client *tmp;
    wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
        client_destroy(client);
    }
    if (server->ipc_source) {
        wl_event_source_remove(server->ipc_source);
        server->ipc_source = NULL;
    }
    if (server->ipc_fd >= 0) {
        close(server->ipc_fd);
        server->ipc_fd = -1;
    }
    if (server->ipc_path) {
        unlink(server->ipc_path);
        free(server->ipc_path);
        server->ipc_path = NULL;
    }
}
