// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland 壁纸协议族：wallpaper_manager（设置面板 → 合成器）+
 * wallpaper_shell / wallpaper_notifier（壁纸客户端 ⇐ 合成器）。
 *
 * 职责边界（对齐 treeland 的现状，见 docs/wallpaper-protocols.md）：
 *
 *   - manager 只做**路径级**校验 + 记配置 + 回 changed/failed。合成器不解码
 *     任何图片/视频——解码在 treeland-wallpaper-factory 里（图片用 Qt、
 *     视频用 mpv），这里连一个解码库都不链。
 *   - shell 给 wl_surface 赋"壁纸角色"，把它挂到场景**最底层**的
 *     wallpaper_tree 上（layer-shell 的 background 层之下），并按锁屏状态
 *     发 pause/play。
 *   - notifier 把"当前有哪些可用壁纸源"广播给壁纸客户端；这份列表完全由
 *     配置推导，不额外维护——否则两处状态迟早不一致。
 *
 * 两个协议都没有"按输出"的壁纸表面：get_treeland_wallpaper_surface 不带
 * output 参数，所以一个壁纸 surface 是**全局铺满**的。per-output 不同壁纸
 * 留给协议扩展，v1 不做。
 */
#include "util/listener.h"
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "effect.h"
#include "output.h"
#include "protocol_internal.h"
#include "session_lock.h"
#include "wallpaper_internal.h"
#include "woodland.h"
#include "workspace.h"

#include "treeland-wallpaper-manager-unstable-v1-protocol.h"
#include "treeland-wallpaper-shell-unstable-v1-protocol.h"

/* 协议定义是 experimental：版本号写死 1，上游升版要显式跟 */
#define WALLPAPER_VERSION 1

/* ==================== 状态 ==================== */

struct woodland_proto_wallpaper;

/* 一个 notifier 客户端 */
struct woodland_wallpaper_notifier {
    struct woodland_proto_wallpaper *wp;
    struct wl_resource *resource;
    struct wl_list link; // woodland_proto_wallpaper.notifiers
};

/* 一个壁纸表面（shell 族）：wl_surface 上挂的角色 */
struct woodland_wallpaper_surface {
    struct woodland_proto_wallpaper *wp;
    struct wl_resource *resource;
    struct wlr_surface *wlr_surface;
    struct wlr_scene_surface *scene_surface;
    char *source;
    bool ready;
    /*
     * 我们**最后告诉客户端**的播放状态：-1 = 还没说过，0 = pause，1 = play。
     *
     * 记一份而不是每次重算：可见性会在 workspace 切换、锁屏、配置变更、
     * 输出热插拔上重算，其中大多数时候结论没变——不比对就会给客户端刷
     * 一串重复的 play/pause，视频端的调速逻辑会被反复打断。
     *
     * 用三态而不是 bool：客户端需要**明确的首帧告知**。只发 play 而不发
     * pause 的话，"这张壁纸当前不该播"这个结论要靠客户端自己猜。
     */
    int play_state;
    struct wl_list link; // woodland_proto_wallpaper.surfaces
    struct wl_listener surface_destroy;
    struct wl_listener surface_commit;
};

/* 一个 manager 对象（treeland_wallpaper_v1）：绑定到某个输出 */
struct woodland_wallpaper {
    struct woodland_proto_wallpaper *wp;
    struct wl_resource *resource;
    struct wlr_output *output; // 可能已销毁（→ NULL），只用来防悬挂
    char *output_name;         // 输出销毁后仍能定位配置
    struct wl_list link;       // woodland_proto_wallpaper.wallpapers
    struct wl_listener output_destroy;
};

struct woodland_proto_wallpaper {
    struct woodland_server *server;

    struct woodland_wallpaper_config config;
    struct wlr_scene_tree *scene_tree; // 场景最底层，装所有壁纸 surface

    struct wl_global *manager_global;
    struct wl_global *shell_global;
    struct wl_global *notifier_global;
    struct wl_list notifiers;  // struct woodland_wallpaper_notifier.link
    struct wl_list surfaces;   // struct woodland_wallpaper_surface.link
    struct wl_list wallpapers; // struct woodland_wallpaper.link

    struct wl_listener output_removed;
    struct wl_listener lock_changed;
    struct wl_listener workspace_changed;
    struct wl_listener layout_change;
};

/* 配置里出现的、当前确实能用的源（用于 notifier 广播与 already_used 判定） */
struct wp_source {
    char *path;
    int type;
};

/* ==================== 小工具 ==================== */

static bool output_connected(struct woodland_server *server, const char *name) {
    if (!name) {
        return false;
    }
    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output && strcmp(output->wlr_output->name, name) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * 当前 workspace 的下标。
 *
 * workspace 名字是 "1".."10"（workspace.c 里按 1 起编），而协议里
 * workspaceIndex 是 0 起的，所以这里减一——弄错就会在 changed 里报错一张图。
 */
static int current_workspace_index(struct woodland_server *server) {
    if (!server->active_workspace || !server->active_workspace->name) {
        return 0;
    }
    int index = atoi(server->active_workspace->name) - 1;
    return index < 0 ? 0 : index;
}

static size_t workspace_count(struct woodland_server *server) {
    return (size_t)wl_list_length(&server->workspaces);
}

/* ==================== 源集合 ==================== */

static void sources_finish(struct wp_source *sources, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(sources[i].path);
    }
    free(sources);
}

static bool sources_add(struct wp_source **sources, size_t *n, size_t *cap,
                        const char *path, int type) {
    if (!path || path[0] == '\0') {
        return true;
    }
    for (size_t i = 0; i < *n; i++) {
        if (strcmp((*sources)[i].path, path) == 0) {
            return true; // 同一个文件被多个 output/workspace 引用是常态
        }
    }
    if (*n == *cap) {
        size_t want = *cap ? *cap * 2 : 8;
        struct wp_source *grown = realloc(*sources, want * sizeof(*grown));
        if (!grown) {
            return false;
        }
        *sources = grown;
        *cap = want;
    }
    char *copy = strdup(path);
    if (!copy) {
        return false;
    }
    (*sources)[*n].path = copy;
    (*sources)[*n].type = type;
    (*n)++;
    return true;
}

/*
 * 收集"当前可用的壁纸源"。
 *
 * 只算**已连接输出**的配置：treeland 的 globalValidWallpaper 就是这个口径，
 * 好处是拔掉的显示器不会继续给客户端留在列表里。失败的分配不影响整体，
 * 少广播一个源比丢一条通知更无害。
 */
static void sources_collect(struct woodland_proto_wallpaper *wp, struct wp_source **out,
                            size_t *out_n) {
    struct wp_source *sources = NULL;
    size_t n = 0, cap = 0;

    for (size_t i = 0; i < wp->config.n_outputs; i++) {
        const struct woodland_wallpaper_output *output = &wp->config.outputs[i];
        if (!output_connected(wp->server, output->name)) {
            continue;
        }
        sources_add(&sources, &n, &cap, output->lockscreen, output->lockscreen_type);
        for (size_t k = 0; k < output->n_workspaces; k++) {
            sources_add(&sources, &n, &cap, output->workspaces[k].desktop,
                        output->workspaces[k].desktop_type);
        }
    }
    *out = sources;
    *out_n = n;
}

static bool source_is_used(struct woodland_proto_wallpaper *wp, const char *path) {
    struct wp_source *sources = NULL;
    size_t n = 0;
    sources_collect(wp, &sources, &n);
    bool used = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sources[i].path, path) == 0) {
            used = true;
            break;
        }
    }
    sources_finish(sources, n);
    return used;
}

/* ==================== 可见性与播放 ==================== */

/*
 * 挑一份"参照输出"的配置。
 *
 * 协议里壁纸 surface 不带 output 参数，是全局铺满的一张，所以只需要一份
 * 配置。优先取**已连接**的输出：配置文件里可能还留着上次会话拔掉的那块屏，
 * 按它挑源会挑到一张谁都没画的壁纸，桌面反而空白。
 */
static const struct woodland_wallpaper_output *
wp_primary_output(struct woodland_proto_wallpaper *wp) {
    for (size_t i = 0; i < wp->config.n_outputs; i++) {
        if (output_connected(wp->server, wp->config.outputs[i].name)) {
            return &wp->config.outputs[i];
        }
    }
    return wp->config.n_outputs ? &wp->config.outputs[0] : NULL;
}

/* 此刻该显示哪个源：锁屏期间看锁屏项，否则看当前 workspace 的桌面项。 */
static const char *wp_desired_source(struct woodland_proto_wallpaper *wp) {
    const struct woodland_wallpaper_output *output = wp_primary_output(wp);
    if (!output) {
        return NULL;
    }
    if (woodland_session_lock_is_locked(wp->server)) {
        return output->lockscreen;
    }
    return woodland_wallpaper_config_desktop(output, current_workspace_index(wp->server),
                                             NULL);
}

/*
 * 重算"哪张壁纸可见"，顺带把看不见的那些暂停。
 *
 * treeland-wallpaper-factory 是**每个源开一个窗口**（收到 notifier 的 add 就
 * 建一个），这些 surface 全被我们挂在 wallpaper_tree 的同一位置，谁后建谁在
 * 上面。不去挑，切换 workspace 就完全没反应、十个 workspace 的视频还会一起
 * 解码。所以这里按"当前 workspace / 锁屏状态"算出唯一该显示的那个源。
 *
 * 一个都匹配不上时（客户端还没为这个源建 surface，或者配置为空）**全部显示**
 * 而不是全隐藏：宁可多显示一张，也不要把桌面变成黑的。
 */
static void wallpaper_apply_visibility(struct woodland_proto_wallpaper *wp) {
    const char *desired = wp_desired_source(wp);
    bool have_match = false;
    if (desired && desired[0]) {
        struct woodland_wallpaper_surface *wallpaper;
        wl_list_for_each(wallpaper, &wp->surfaces, link) {
            if (wallpaper->source && strcmp(wallpaper->source, desired) == 0) {
                have_match = true;
                break;
            }
        }
    }

    struct woodland_wallpaper_surface *wallpaper;
    wl_list_for_each(wallpaper, &wp->surfaces, link) {
        bool visible = !have_match ||
                       (desired && wallpaper->source && strcmp(wallpaper->source, desired) == 0);
        bool show = visible && wallpaper->ready;
        if (wallpaper->scene_surface) {
            struct wlr_scene_node *node = &wallpaper->scene_surface->buffer->node;
            if (node->enabled != show) {
                wlr_scene_node_set_enabled(node, show);
                woodland_effect_mark_blur_dirty(wp->server);
            }
        }
        if (!wallpaper->ready || wallpaper->play_state == (visible ? 1 : 0)) {
            continue;
        }
        /* 隐藏的那张不是"不存在"，它只是看不见：发 pause 让它别再解码 */
        if (visible) {
            treeland_wallpaper_surface_v1_send_play(wallpaper->resource);
        } else {
            treeland_wallpaper_surface_v1_send_pause(wallpaper->resource);
        }
        wallpaper->play_state = visible ? 1 : 0;
    }
}

/* ==================== notifier ==================== */

static void notifier_send_add(struct woodland_wallpaper_notifier *notifier, int type,
                              const char *path) {
    treeland_wallpaper_notifier_v1_send_add(notifier->resource, (uint32_t)type, path);
}

static void notifier_broadcast_add(struct woodland_proto_wallpaper *wp, int type,
                                   const char *path) {
    struct woodland_wallpaper_notifier *notifier;
    wl_list_for_each(notifier, &wp->notifiers, link) {
        notifier_send_add(notifier, type, path);
    }
}

static void notifier_broadcast_remove(struct woodland_proto_wallpaper *wp,
                                      const char *path) {
    struct woodland_wallpaper_notifier *notifier;
    wl_list_for_each(notifier, &wp->notifiers, link) {
        treeland_wallpaper_notifier_v1_send_remove(notifier->resource, path);
    }
}

static void notifier_resource_destroy(struct wl_resource *resource) {
    struct woodland_wallpaper_notifier *notifier = wl_resource_get_user_data(resource);
    if (!notifier) {
        return;
    }
    wl_list_remove(&notifier->link);
    free(notifier);
}

static void notifier_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct treeland_wallpaper_notifier_v1_interface notifier_impl = {
    .destroy = notifier_handle_destroy,
};

/*
 * notifier 挂在 shell 全局上（协议把它和 shell 放在同一个 XML 里），但它是
 * 独立接口：客户端 bind 的是 treeland_wallpaper_notifier_v1 这个名字。
 */
static void notifier_bind(struct wl_client *client, void *data, uint32_t version,
                          uint32_t id) {
    struct woodland_proto_wallpaper *wp = data;
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_wallpaper_notifier_v1_interface,
                           (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct woodland_wallpaper_notifier *notifier = calloc(1, sizeof(*notifier));
    if (!notifier) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    notifier->wp = wp;
    notifier->resource = resource;
    wl_list_insert(&wp->notifiers, &notifier->link);
    wl_resource_set_implementation(resource, &notifier_impl, notifier,
                                   notifier_resource_destroy);

    /* bind 时重放一遍：先连上的客户端和后来的客户端看到同一份列表 */
    struct wp_source *sources = NULL;
    size_t n = 0;
    sources_collect(wp, &sources, &n);
    for (size_t i = 0; i < n; i++) {
        notifier_send_add(notifier, sources[i].type, sources[i].path);
    }
    sources_finish(sources, n);
}

/* ==================== shell：壁纸角色 ==================== */

/*
 * 壁纸角色。
 *
 * 只需要名字：渲染交给 scene（wlr_scene_surface 自己跟 buffer 提交），尺寸由
 * 客户端决定（它按屏幕大小画），合成器不参与布局。
 *
 * 不用 role.destroy 做回收：它只拿得到 wlr_surface，拿不到我们的包装结构；
 * 而资源的析构函数拿得到 user_data，一处收干净更不容易漏。
 */
static const struct wlr_surface_role wallpaper_role = {
    .name = "treeland_wallpaper_surface_v1",
};

static void wallpaper_surface_free(struct woodland_wallpaper_surface *wallpaper) {
    if (wallpaper->surface_commit.link.prev) {
        wl_list_remove(&wallpaper->surface_commit.link);
    }
    if (wallpaper->surface_destroy.link.prev) {
        wl_list_remove(&wallpaper->surface_destroy.link);
    }
    wl_list_remove(&wallpaper->link);
    free(wallpaper->source);
    free(wallpaper);
}

static void wallpaper_handle_surface_commit(struct wl_listener *listener, void *data) {
    struct woodland_wallpaper_surface *wallpaper =
        wl_container_of(listener, wallpaper, surface_commit);
    (void)data;
    if (wallpaper->ready && wallpaper->scene_surface &&
        wallpaper->scene_surface->buffer->node.enabled) {
        woodland_effect_mark_blur_dirty(wallpaper->wp->server);
    }
}

static void wallpaper_handle_surface_destroy(struct wl_listener *listener, void *data) {
    struct woodland_wallpaper_surface *wallpaper =
        wl_container_of(listener, wallpaper, surface_destroy);
    (void)data;
    /* wlroots 的 scene_surface 会在其自己的 surface.events.destroy 监听器中释放节点 */
    wallpaper->wlr_surface = NULL;
    wallpaper->scene_surface = NULL;
    wl_list_remove(&wallpaper->surface_commit.link);
    wl_list_remove(&wallpaper->surface_destroy.link);
    wl_list_init(&wallpaper->surface_commit.link);
    wl_list_init(&wallpaper->surface_destroy.link);
}

static void wallpaper_surface_resource_destroy(struct wl_resource *resource) {
    struct woodland_wallpaper_surface *wallpaper = wl_resource_get_user_data(resource);
    if (!wallpaper) {
        return;
    }
    /* 先摘节点：角色没了但 surface 还活着时，别让旧内容继续铺在桌面上 */
    if (wallpaper->scene_surface) {
        wlr_scene_node_destroy(&wallpaper->scene_surface->buffer->node);
        wallpaper->scene_surface = NULL;
    }
    wallpaper_surface_free(wallpaper);
}

static void wallpaper_surface_handle_destroy(struct wl_client *client,
                                             struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void wallpaper_surface_handle_ready(struct wl_client *client,
                                           struct wl_resource *resource) {
    (void)client;
    struct woodland_wallpaper_surface *wallpaper = wl_resource_get_user_data(resource);
    if (!wallpaper || wallpaper->ready) {
        return;
    }
    wallpaper->ready = true;
    /*
     * 不在这里无条件 set_enabled(true) + play：这张源未必是当前 workspace
     * （或锁屏）该显示的那张。交给可见性统一裁决，它同时负责首帧的 play。
     */
    wallpaper_apply_visibility(wallpaper->wp);
}

static void wallpaper_surface_handle_source_failed(struct wl_client *client,
                                                   struct wl_resource *resource,
                                                   uint32_t error) {
    (void)client;
    struct woodland_wallpaper_surface *wallpaper = wl_resource_get_user_data(resource);
    if (!wallpaper) {
        return;
    }
    /*
     * 解码在客户端，它说读不出来就是读不出来。合成器这边没有"纯色壁纸"
     * 可以退——layer-shell 的客户端自己画背景，woodland 不垫底色。
     * 记一条日志足够定位问题（客户端 ID + 源 + 错误码）。
     */
    wlr_log(WLR_ERROR, "wallpaper client failed on %s (error %u)",
            wallpaper->source ? wallpaper->source : "?", error);
}

static const struct treeland_wallpaper_surface_v1_interface wallpaper_surface_impl = {
    .destroy = wallpaper_surface_handle_destroy,
    .ready = wallpaper_surface_handle_ready,
    .source_failed = wallpaper_surface_handle_source_failed,
};

static void shell_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void shell_get_wallpaper_surface(struct wl_client *client,
                                        struct wl_resource *resource, uint32_t id,
                                        struct wl_resource *surface_resource,
                                        const char *file_source) {
    struct woodland_proto_wallpaper *wp = wl_resource_get_user_data(resource);
    if (!surface_resource) {
        wl_resource_post_error(resource, 0, "wallpaper surface is NULL");
        return;
    }
    struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);
    if (!wlr_surface) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "invalid wl_surface");
        return;
    }

    struct wl_resource *wallpaper_resource =
        wl_resource_create(client, &treeland_wallpaper_surface_v1_interface,
                           wl_resource_get_version(resource), id);
    if (!wallpaper_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct woodland_wallpaper_surface *wallpaper = calloc(1, sizeof(*wallpaper));
    if (!wallpaper) {
        wl_resource_destroy(wallpaper_resource);
        wl_client_post_no_memory(client);
        return;
    }
    wallpaper->wp = wp;
    wallpaper->resource = wallpaper_resource;
    wallpaper->wlr_surface = wlr_surface;
    /* 还没告诉过客户端该播还是该停（0 会被当成"已经说过 pause"） */
    wallpaper->play_state = -1;
    wl_list_init(&wallpaper->link);
    wl_list_init(&wallpaper->surface_destroy.link);
    /* 先挂析构再干活：后面任何一步失败都靠 wl_resource_destroy 统一收尾 */
    wl_resource_set_implementation(wallpaper_resource, &wallpaper_surface_impl,
                                   wallpaper, wallpaper_surface_resource_destroy);

    wallpaper->source = strdup(file_source ? file_source : "");
    if (!wallpaper->source) {
        wl_resource_destroy(wallpaper_resource);
        return;
    }

    /*
     * 角色赋值失败（surface 已经属于别人）由 wlr 直接给客户端发协议错误，
     * 我们只需要把刚建的资源收掉。
     */
    if (!wlr_surface_set_role(wlr_surface, &wallpaper_role, wallpaper_resource, 0)) {
        wl_resource_destroy(wallpaper_resource);
        return;
    }
    wlr_surface_set_role_object(wlr_surface, wallpaper_resource);

    wallpaper->scene_surface = wlr_scene_surface_create(wp->scene_tree, wlr_surface);
    if (!wallpaper->scene_surface) {
        wlr_log(WLR_ERROR, "failed to create scene surface for wallpaper");
        wl_resource_destroy(wallpaper_resource);
        return;
    }
    /* 位置归零：树本身已经压在 scene 最底，节点的位置只是屏内偏移 */
    wlr_scene_node_set_position(&wallpaper->scene_surface->buffer->node, 0, 0);
    /* ready 之前不画：否则会闪一帧上一次的内容（协议里就是这么约定的） */
    wlr_scene_node_set_enabled(&wallpaper->scene_surface->buffer->node, false);

    woodland_listener_connect(&wallpaper->surface_destroy, &wlr_surface->events.destroy,
                              wallpaper_handle_surface_destroy, "wallpaper.surface_destroy");
    woodland_listener_connect(&wallpaper->surface_commit, &wlr_surface->events.commit,
                              wallpaper_handle_surface_commit, "wallpaper.surface_commit");
    wl_list_insert(&wp->surfaces, &wallpaper->link);
    /* 新 surface 可能不是当前该显示的那张：先让可见性裁决一次再报日志 */
    wallpaper_apply_visibility(wp);
    wlr_log(WLR_INFO, "wallpaper surface for %s created", wallpaper->source);
}

static const struct treeland_wallpaper_shell_v1_interface shell_impl = {
    .destroy = shell_handle_destroy,
    .get_treeland_wallpaper_surface = shell_get_wallpaper_surface,
};

static void shell_bind(struct wl_client *client, void *data, uint32_t version,
                       uint32_t id) {
    struct woodland_proto_wallpaper *wp = data;
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_wallpaper_shell_v1_interface,
                           (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    /*
     * 协议写着"至多一个客户端可以 bind"，但 treeland 不强制（见规格 §11.6）。
     * 我们也不强制：多开一个壁纸进程是用户自己的事，抢同一块屏幕时 z 序会
     * 自己给出答案，比在 bind 处报错更容易排查。
     */
    wl_resource_set_implementation(resource, &shell_impl, wp, NULL);
}

/* ==================== manager：配置 ==================== */

static void wallpaper_send_changed(struct woodland_wallpaper *wallpaper,
                                   enum woodland_wallpaper_role role, int type,
                                   const char *path) {
    if (!path || path[0] == '\0') {
        return;
    }
    treeland_wallpaper_v1_send_changed(wallpaper->resource, (uint32_t)role,
                                       (uint32_t)type, path);
}

static void wallpaper_send_failed(struct woodland_wallpaper *wallpaper, const char *path,
                                  uint32_t error) {
    treeland_wallpaper_v1_send_failed(wallpaper->resource, path, error);
}

/*
 * 新建 manager 对象时把当前配置回给客户端（对齐 treeland 的
 * onWallpaperAdded）：锁屏一项 + 当前 workspace 的桌面一项。
 *
 * 注意这里**只**在创建时发；set_*_source 之后 treeland 不再回 changed
 * （它靠 updateWallpaper / notifier 让壁纸进程重画），照抄以免客户端以为
 * "我设的值被别人改了"。
 */
static void wallpaper_send_current(struct woodland_wallpaper *wallpaper) {
    struct woodland_wallpaper_config *config = &wallpaper->wp->config;
    struct woodland_wallpaper_output *output =
        woodland_wallpaper_config_find_output(config, wallpaper->output_name);
    if (!output) {
        return;
    }
    wallpaper_send_changed(wallpaper, WOODLAND_WALLPAPER_LOCKSCREEN,
                           output->lockscreen_type, output->lockscreen);

    int type = WOODLAND_WALLPAPER_IMAGE;
    const char *desktop = woodland_wallpaper_config_desktop(
        output, current_workspace_index(wallpaper->wp->server), &type);
    wallpaper_send_changed(wallpaper, WOODLAND_WALLPAPER_DESKTOP, type, desktop);
}

static void wallpaper_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void wallpaper_resource_destroy(struct wl_resource *resource) {
    struct woodland_wallpaper *wallpaper = wl_resource_get_user_data(resource);
    if (!wallpaper) {
        return;
    }
    if (wallpaper->output_destroy.link.prev) {
        wl_list_remove(&wallpaper->output_destroy.link);
        wl_list_init(&wallpaper->output_destroy.link);
    }
    wl_list_remove(&wallpaper->link);
    free(wallpaper->output_name);
    free(wallpaper);
}

static void wallpaper_handle_output_destroy(struct wl_listener *listener, void *data) {
    struct woodland_wallpaper *wallpaper =
        wl_container_of(listener, wallpaper, output_destroy);
    (void)data;
    wallpaper->output = NULL;
    /*
     * 协议要求客户端先销毁 wallpaper 再销毁 output。客户端不讲理时不能留
     * 悬挂指针：直接收掉对象，客户端下一次调用会得到 "invalid object"。
     */
    wl_resource_destroy(wallpaper->resource);
}

static void wallpaper_apply_source(struct wl_resource *resource, const char *file_source,
                                   uint32_t role, enum woodland_wallpaper_type type) {
    struct woodland_wallpaper *wallpaper = wl_resource_get_user_data(resource);
    if (!wallpaper) {
        return;
    }
    struct woodland_proto_wallpaper *wp = wallpaper->wp;

    enum woodland_wallpaper_source check = woodland_wallpaper_source_check(file_source);
    if (check != WOODLAND_WALLPAPER_SOURCE_OK) {
        wallpaper_send_failed(wallpaper, file_source ? file_source : "", (uint32_t)check);
        return;
    }
    if ((role & (WOODLAND_WALLPAPER_DESKTOP | WOODLAND_WALLPAPER_LOCKSCREEN)) == 0) {
        wallpaper_send_failed(wallpaper, file_source,
                              TREELAND_WALLPAPER_V1_ERROR_INVALID_SOURCE);
        return;
    }

    /*
     * already_used 必须在写入**之前**判定：写入之后这个源就一定"在用"了，
     * 那样每次设置都会报 already_used。
     */
    bool already_used = source_is_used(wp, file_source);

    if (woodland_wallpaper_config_set_source(&wp->config, wallpaper->output_name,
                                             (enum woodland_wallpaper_role)role, type,
                                             file_source)) {
        woodland_wallpaper_config_save(&wp->config);
        /*
         * 改的可能正是"当前这张"：立刻重挑一次。新源对应的 surface 还没
         * 建出来（客户端要先收到 notifier 的 add），这一趟会退化成"全显示"，
         * 等 surface 到位后再由 ready 收敛——中间不会出现黑屏。
         */
        wallpaper_apply_visibility(wp);
    }

    if (already_used) {
        /* 非致命：客户端拿它当"这个图已经在缓存里"的提示（treeland 同语义） */
        wallpaper_send_failed(wallpaper, file_source,
                              TREELAND_WALLPAPER_V1_ERROR_ALREADY_USED);
    } else {
        notifier_broadcast_add(wp, (int)type, file_source);
    }
}

static void wallpaper_set_image_source(struct wl_client *client, struct wl_resource *resource,
                                       const char *file_source, uint32_t role) {
    (void)client;
    wallpaper_apply_source(resource, file_source, role, WOODLAND_WALLPAPER_IMAGE);
}

static void wallpaper_set_video_source(struct wl_client *client, struct wl_resource *resource,
                                       const char *file_source, uint32_t role) {
    (void)client;
    /* 类型由"用了哪个请求"决定，不做内容嗅探：解码不在合成器里 */
    wallpaper_apply_source(resource, file_source, role, WOODLAND_WALLPAPER_VIDEO);
}

static const struct treeland_wallpaper_v1_interface wallpaper_impl = {
    .destroy = wallpaper_handle_destroy,
    .set_image_source = wallpaper_set_image_source,
    .set_video_source = wallpaper_set_video_source,
};

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_get_wallpaper(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *output_resource,
                                  struct wl_resource *surface_resource) {
    (void)surface_resource; // v1 只记录"有引用 surface"，不参与判定
    struct woodland_proto_wallpaper *wp = wl_resource_get_user_data(resource);

    struct wlr_output *output = wlr_output_from_resource(output_resource);
    if (!output) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "invalid wl_output");
        return;
    }

    struct wl_resource *wallpaper_resource =
        wl_resource_create(client, &treeland_wallpaper_v1_interface,
                           wl_resource_get_version(resource), id);
    if (!wallpaper_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct woodland_wallpaper *wallpaper = calloc(1, sizeof(*wallpaper));
    if (!wallpaper) {
        wl_resource_destroy(wallpaper_resource);
        wl_client_post_no_memory(client);
        return;
    }
    wallpaper->wp = wp;
    wallpaper->resource = wallpaper_resource;
    wallpaper->output = output;
    wl_list_init(&wallpaper->link);
    wl_list_init(&wallpaper->output_destroy.link);
    /* 先挂析构再分配字段：中途失败统一走 wl_resource_destroy */
    wl_resource_set_implementation(wallpaper_resource, &wallpaper_impl, wallpaper,
                                   wallpaper_resource_destroy);

    wallpaper->output_name = strdup(output->name);
    if (!wallpaper->output_name) {
        wl_resource_destroy(wallpaper_resource);
        return;
    }

    /* 没有配置的输出先补一份（克隆已有输出），否则桌面壁纸是空的 */
    bool created = false;
    woodland_wallpaper_config_ensure_output(&wp->config, output->name,
                                           workspace_count(wp->server), &created);
    if (created) {
        woodland_wallpaper_config_save(&wp->config);
    }

    woodland_listener_connect(&wallpaper->output_destroy, &output->events.destroy,
                              wallpaper_handle_output_destroy, "wallpaper.output_destroy");
    wl_list_insert(&wp->wallpapers, &wallpaper->link);

    wallpaper_send_current(wallpaper);
}

static const struct treeland_wallpaper_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_treeland_wallpaper = manager_get_wallpaper,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
                         uint32_t id) {
    struct woodland_proto_wallpaper *wp = data;
    struct wl_resource *resource =
        wl_resource_create(client, &treeland_wallpaper_manager_v1_interface,
                           (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, wp, NULL);
}

/* ==================== 输出热插拔 ==================== */

static void wallpaper_ensure_output(struct woodland_proto_wallpaper *wp,
                                    struct wlr_output *output) {
    if (!output || output->non_desktop) {
        return;
    }
    bool created = false;
    struct woodland_wallpaper_output *output_cfg = woodland_wallpaper_config_ensure_output(
        &wp->config, output->name, workspace_count(wp->server), &created);
    if (!output_cfg) {
        return;
    }
    if (created) {
        woodland_wallpaper_config_save(&wp->config);
    }

    /*
     * 不管配置是不是新建的都要广播 add：显示器拔掉时我们发过 remove，
     * 插回来（配置项还在，created 为假）必须重新告诉客户端"这个源又能用了"，
     * 否则壁纸进程会一直以为那块屏幕没内容。重复 add 是无害的。
     */
    struct wp_source *sources = NULL;
    size_t n = 0, cap = 0;
    sources_add(&sources, &n, &cap, output_cfg->lockscreen, output_cfg->lockscreen_type);
    for (size_t i = 0; i < output_cfg->n_workspaces; i++) {
        sources_add(&sources, &n, &cap, output_cfg->workspaces[i].desktop,
                    output_cfg->workspaces[i].desktop_type);
    }
    for (size_t i = 0; i < n; i++) {
        notifier_broadcast_add(wp, sources[i].type, sources[i].path);
    }
    sources_finish(sources, n);
}

static void wallpaper_handle_output_removed(struct wl_listener *listener, void *data) {
    struct woodland_proto_wallpaper *wp =
        wl_container_of(listener, wp, output_removed);
    struct wlr_output *output = data;

    struct woodland_wallpaper_output *config =
        woodland_wallpaper_config_find_output(&wp->config, output->name);
    if (!config) {
        return;
    }
    /*
     * 输出拔了但配置留着（插回来还是原来那张图）。先去重再广播 remove，
     * 否则每个 workspace 都填同一张壁纸会连续发出多次重复的 remove；
     * 且只有"再没有别的输出引用这个源"时才通知 remove。
     */
    struct wp_source *sources = NULL;
    size_t n = 0, cap = 0;
    sources_add(&sources, &n, &cap, config->lockscreen, config->lockscreen_type);
    for (size_t i = 0; i < config->n_workspaces; i++) {
        sources_add(&sources, &n, &cap, config->workspaces[i].desktop,
                    config->workspaces[i].desktop_type);
    }
    for (size_t i = 0; i < n; i++) {
        if (!source_is_used(wp, sources[i].path)) {
            notifier_broadcast_remove(wp, sources[i].path);
        }
    }
    sources_finish(sources, n);
    /* 参照输出可能换人了（拔掉的正是主屏）：重新挑一次可见的源 */
    wallpaper_apply_visibility(wp);
}

/* ==================== 播放控制 ==================== */

static void wallpaper_handle_lock_changed(struct wl_listener *listener, void *data) {
    struct woodland_proto_wallpaper *wp =
        wl_container_of(listener, wp, lock_changed);
    (void)data;
    /*
     * 信号在锁定状态已经落定之后才发，直接问状态比传值少一层约定。
     * 锁屏只改"该显示哪张"：锁屏期间是锁屏源，解锁后回到当前 workspace
     * 的桌面源——正好是可见性函数的那两个分支，不用在这里各写一遍。
     */
    wallpaper_apply_visibility(wp);
}

/*
 * 切了工作区。
 *
 * 回调里不读 data：切换在发信号之前就已经落定（server->active_workspace
 * 已是新值），直接问比再传一遍少一层"万一传的和问的不一样"。
 */
static void wallpaper_handle_workspace_changed(struct wl_listener *listener, void *data) {
    struct woodland_proto_wallpaper *wp =
        wl_container_of(listener, wp, workspace_changed);
    (void)data;
    wallpaper_apply_visibility(wp);
}
static void wallpaper_update_position(struct woodland_proto_wallpaper *wp) {
    if (!wp || !wp->scene_tree || !wp->server->output_layout) {
        return;
    }
    struct wlr_box box;
    wlr_output_layout_get_box(wp->server->output_layout, NULL, &box);
    wlr_scene_node_set_position(&wp->scene_tree->node, box.x, box.y);
}

static void wallpaper_handle_layout_change(struct wl_listener *listener, void *data) {
    struct woodland_proto_wallpaper *wp =
        wl_container_of(listener, wp, layout_change);
    (void)data;
    wallpaper_update_position(wp);
    woodland_effect_outputs_update(wp->server);
}


/* ==================== 由编排层调用的入口 ==================== */

void woodland_proto_wallpaper_output_added(struct woodland_proto *proto,
                                           struct wlr_output *output) {
    struct woodland_proto_wallpaper *wp = proto ? proto->wallpaper : NULL;
    if (!wp || !wp->scene_tree) {
        return;
    }
    /*
     * 图层树是 lower_to_bottom() 建的（output.c），会把壁纸树顶上去；
     * 每来一个输出就把基线重新压回最底，保证壁纸永远在 layer-shell
     * background 之下。
     */
    wlr_scene_node_lower_to_bottom(&wp->scene_tree->node);

    /* 多屏时把壁纸挂到布局左上角，别只在主屏原点铺一块 */
    wallpaper_update_position(wp);

    wallpaper_ensure_output(wp, output);
    wallpaper_apply_visibility(wp);
}

bool woodland_proto_wallpaper_init(struct woodland_proto *proto) {
    struct woodland_server *server = proto->server;

    struct woodland_proto_wallpaper *wp = calloc(1, sizeof(*wp));
    if (!wp) {
        wlr_log(WLR_ERROR, "cannot allocate wallpaper protocol state");
        return false;
    }
    wp->server = server;
    woodland_wallpaper_config_init(&wp->config);
    wl_list_init(&wp->notifiers);
    wl_list_init(&wp->surfaces);
    wl_list_init(&wp->wallpapers);
    wl_list_init(&wp->output_removed.link);
    wl_list_init(&wp->lock_changed.link);
    wl_list_init(&wp->workspace_changed.link);
    wl_list_init(&wp->layout_change.link);
    /* 先挂上去：后面任何一步失败，finish 都能顺着它回收 */
    proto->wallpaper = wp;

    /* 配置读不出来（首次启动）就先空着，输出出现时补默认值 */
    woodland_wallpaper_config_load(&wp->config);

    wp->scene_tree = wlr_scene_tree_create(&server->scene->tree);
    if (!wp->scene_tree) {
        wlr_log(WLR_ERROR, "failed to create wallpaper scene tree");
        return false;
    }
    wlr_scene_node_lower_to_bottom(&wp->scene_tree->node);
    wallpaper_update_position(wp);
    /* self_check 靠它断言"壁纸永远在最底层"（shell 库不链接协议库） */
    server->wallpaper_tree = wp->scene_tree;

    wp->manager_global = wl_global_create(server->display,
                                          &treeland_wallpaper_manager_v1_interface,
                                          WALLPAPER_VERSION, wp, manager_bind);
    wp->shell_global = wl_global_create(server->display,
                                        &treeland_wallpaper_shell_v1_interface,
                                        WALLPAPER_VERSION, wp, shell_bind);
    wp->notifier_global = wl_global_create(server->display,
                                           &treeland_wallpaper_notifier_v1_interface,
                                           WALLPAPER_VERSION, wp, notifier_bind);
    if (!wp->manager_global || !wp->shell_global || !wp->notifier_global) {
        wlr_log(WLR_ERROR, "failed to create wallpaper globals");
        return false;
    }

    woodland_listener_connect(&wp->output_removed, &server->output_removed,
                              wallpaper_handle_output_removed,
                              "wallpaper.output_removed");
    if (server->session_lock) {
        woodland_listener_connect(&wp->lock_changed, &server->session_lock->lock_changed,
                                  wallpaper_handle_lock_changed,
                                  "wallpaper.lock_changed");
    }
    woodland_listener_connect(&wp->workspace_changed, &server->workspace_changed,
                              wallpaper_handle_workspace_changed,
                              "wallpaper.workspace_changed");
    woodland_listener_connect(&wp->layout_change, &server->output_layout->events.change,
                              wallpaper_handle_layout_change,
                              "wallpaper.layout_change");


    /* headless 输出早于协议层建好：这里补一次配置，别等下一次热插拔 */
    struct woodland_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        wallpaper_ensure_output(wp, output->wlr_output);
    }

    return true;
}

void woodland_proto_wallpaper_finish(struct woodland_proto *proto) {
    struct woodland_proto_wallpaper *wp = proto->wallpaper;
    if (!wp) {
        return;
    }

    if (wp->output_removed.link.prev) {
        wl_list_remove(&wp->output_removed.link);
    }
    if (wp->lock_changed.link.prev) {
        wl_list_remove(&wp->lock_changed.link);
    }
    if (wp->workspace_changed.link.prev) {
        wl_list_remove(&wp->workspace_changed.link);
    }
    if (wp->layout_change.link.prev) {
        wl_list_remove(&wp->layout_change.link);
    }
    if (wp->manager_global) {
        wl_global_destroy(wp->manager_global);
        wp->manager_global = NULL;
    }
    if (wp->shell_global) {
        wl_global_destroy(wp->shell_global);
        wp->shell_global = NULL;
    }
    if (wp->notifier_global) {
        wl_global_destroy(wp->notifier_global);
        wp->notifier_global = NULL;
    }

    /*
     * 客户端资源正常都已随 display 销毁；这里兜底释放包装结构。先把
     * user_data 置空，万一还有活着的资源，它的析构函数不会再二次 free。
     */
    struct woodland_wallpaper_notifier *notifier, *n_tmp;
    wl_list_for_each_safe(notifier, n_tmp, &wp->notifiers, link) {
        wl_list_remove(&notifier->link);
        if (notifier->resource) {
            wl_resource_set_user_data(notifier->resource, NULL);
        }
        free(notifier);
    }
    struct woodland_wallpaper_surface *surface, *s_tmp;
    wl_list_for_each_safe(surface, s_tmp, &wp->surfaces, link) {
        if (surface->surface_destroy.link.prev) {
            wl_list_remove(&surface->surface_destroy.link);
        }
        wl_list_remove(&surface->link);
        if (surface->resource) {
            wl_resource_set_user_data(surface->resource, NULL);
        }
        free(surface->source);
        free(surface);
    }
    struct woodland_wallpaper *wallpaper, *w_tmp;
    wl_list_for_each_safe(wallpaper, w_tmp, &wp->wallpapers, link) {
        if (wallpaper->output_destroy.link.prev) {
            wl_list_remove(&wallpaper->output_destroy.link);
        }
        wl_list_remove(&wallpaper->link);
        if (wallpaper->resource) {
            wl_resource_set_user_data(wallpaper->resource, NULL);
        }
        free(wallpaper->output_name);
        free(wallpaper);
    }

    if (wp->scene_tree) {
        wlr_scene_node_destroy(&wp->scene_tree->node);
        wp->scene_tree = NULL;
    }
    if (wp->server) {
        wp->server->wallpaper_tree = NULL;
    }

    woodland_wallpaper_config_finish(&wp->config);
    free(wp);
    proto->wallpaper = NULL;
}
