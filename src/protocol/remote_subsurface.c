// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-remote-subsurface-unstable-v1：基于 token 的跨客户端/同客户端子表面协议。
 *
 * 设计要点：
 *   1. export_surface 注册 wl_surface 并生成 128 位随机 hex UUID token，下发
 *      surface_token 事件；同一 wl_surface 不可在当前活跃导出集中重复导出（否则
 *      抛出 manager.error.bad_surface）。
 *   2. create_remote_subsurface 按 parent_token 查找已导出的父表面，校验：
 *      - 空 token / 已挂父关系 / 找不到父 token -> parent_rejected(invalid_token)
 *      - 自引用或父为自身的后代（成环）         -> parent_rejected(cyclic)
 *      - 赋予 exclusive 角色 treeland_remote_subsurface_v1（冲突时抛 exported.error.bad_surface
 *        且令新 remote 对象处于 inert 状态）。
 *   3. 映射与场景树：
 *      - 跨进程子表面恒为 desynchronized 模式，不拥有键盘焦点；
 *      - 当父表面已映射且子表面已提交 buffer 时 wlr_surface_map(child)，父表面
 *        unmap 或关系解除时级联 wlr_surface_unmap(child)；
 *      - 场景树通过 wlr_scene_subsurface_tree_create(parent_tree, child_surface)
 *        惰性挂载在父表面场景树下（父表面可能在 export 之后才绑定角色并进入场景），
 *        并支持按 below_children / parent content / above_children 同步 Z 序；
 *      - 在 child_surface 上通过 woodland_remote_subsurface_set_parent() 登记父表面，
 *        令 woodland_view_from_surface / woodland_layer_surface_at 能穿透 remote
 *        父链回溯到所属顶层窗口。
 *   4. 生命周期解耦：
 *      - 销毁 manager 不影响已导出的对象与现有父子关系；
 *      - 销毁 exported_surface 仅吊销 token（不再接受新的 create_remote_subsurface
 *        引用），不影响已建立的父子关系；底层 wl_surface 销毁或 remote_subsurface
 *        销毁时，卸载子表面树、断开父子关系、销毁场景子树并将关联的 remote_subsurface
 *        置为 inert，等待客户端自行销毁。
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "config.h"
#include "protocol.h"
#include "protocol_internal.h"
#include "scene.h"
#include "trace.h"
#include "util/json.h"
#include "util/listener.h"
#include "woodland.h"

#include "treeland-remote-subsurface-unstable-v1-protocol.h"

#define REMOTE_SUBSURFACE_VERSION 1
#define REMOTE_TOKEN_LEN 36 /* "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" */

struct remote_manager_binding {
    struct woodland_proto_remote_subsurface *family;
    struct wl_resource *resource;
    struct wl_list link; // family->bindings
};

struct remote_subsurface_ctx;

struct exported_surface_ctx {
    struct woodland_proto_remote_subsurface *family;
    struct wl_resource *resource;
    struct wlr_surface *surface;
    char token[REMOTE_TOKEN_LEN + 1];
    bool revoked;
    bool in_surface_destroy;

    /* 当前作为 child 时的关联（至多一个） */
    struct remote_subsurface_ctx *remote_as_child;

    /* 当前作为 parent 时的远程子表面有序链表 */
    struct wl_list below_children; // struct remote_subsurface_ctx.parent_link
    struct wl_list above_children; // struct remote_subsurface_ctx.parent_link

    struct wl_listener surface_destroy;
    struct wl_list link; // family->exported_surfaces
};

struct remote_subsurface_ctx {
    struct woodland_proto_remote_subsurface *family;
    struct wl_resource *resource;
    struct exported_surface_ctx *child;
    struct exported_surface_ctx *parent;
    bool in_below;
    bool attached;
    int32_t x;
    int32_t y;

    struct wlr_scene_tree *scene_tree;
    struct wl_listener scene_tree_destroy;
    struct wl_listener parent_map;
    struct wl_listener parent_unmap;
    struct wl_listener parent_commit;
    struct wl_listener child_client_commit;

    struct wl_list parent_link; // parent->below_children or parent->above_children
    struct wl_list link;        // family->remote_subsurfaces
};

struct woodland_proto_remote_subsurface {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list bindings;          // struct remote_manager_binding.link
    struct wl_list exported_surfaces; // struct exported_surface_ctx.link
    struct wl_list remote_subsurfaces;// struct remote_subsurface_ctx.link

    /* 诊断计数（供 wctl get_tree 观测） */
    uint32_t export_count;
    uint32_t create_count;
    uint32_t reject_count;
    uint32_t invalid_sibling_count;
    uint32_t restack_count;
    uint32_t position_count;
    uint64_t fallback_rng_seq;
};

static void remote_recheck_mapping(struct remote_subsurface_ctx *remote);
static void remote_sync_parent_scene_order(struct exported_surface_ctx *parent);
static void exported_maybe_free(struct exported_surface_ctx *ctx);

/* ==================== Token 生成与查询 ==================== */

static void fill_random_bytes(struct woodland_proto_remote_subsurface *family,
                              uint8_t *buf, size_t len)
{
    ssize_t got = getrandom(buf, len, GRND_NONBLOCK);
    if (got == (ssize_t)len) {
        return;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, len);
        close(fd);
        if (r == (ssize_t)len) {
            return;
        }
    }
    /* 极端沙箱兜底：单调递增计数器混合高精度时钟 */
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t state = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^
                     (uint64_t)(uintptr_t)family ^ (++family->fallback_rng_seq * 0x9E3779B97F4A7C15ULL);
    for (size_t i = 0; i < len; i++) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        buf[i] = (uint8_t)((state * 0x2545F4914F6CDD1DULL) >> 56);
    }
}

static struct exported_surface_ctx *
find_exported_by_token(struct woodland_proto_remote_subsurface *family, const char *token)
{
    if (!family || !token || !token[0]) {
        return NULL;
    }
    struct exported_surface_ctx *ctx;
    wl_list_for_each(ctx, &family->exported_surfaces, link) {
        if (!ctx->revoked && strcmp(ctx->token, token) == 0) {
            return ctx;
        }
    }
    return NULL;
}

static struct exported_surface_ctx *
find_exported_by_surface(struct woodland_proto_remote_subsurface *family,
                         const struct wlr_surface *surface)
{
    if (!family || !surface) {
        return NULL;
    }
    struct exported_surface_ctx *ctx;
    wl_list_for_each(ctx, &family->exported_surfaces, link) {
        if (!ctx->revoked && ctx->surface == surface) {
            return ctx;
        }
    }
    return NULL;
}

static void generate_unique_token(struct woodland_proto_remote_subsurface *family,
                                  char out[static REMOTE_TOKEN_LEN + 1])
{
    for (int attempt = 0; attempt < 16; attempt++) {
        uint8_t b[16];
        fill_random_bytes(family, b, sizeof(b));
        /* RFC 4122 UUID v4 */
        b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
        b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
        snprintf(out, REMOTE_TOKEN_LEN + 1,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        if (!find_exported_by_token(family, out)) {
            return;
        }
    }
}

static bool is_descendant_of(const struct exported_surface_ctx *candidate,
                             const struct exported_surface_ctx *root)
{
    if (!candidate || !root) {
        return false;
    }
    const struct exported_surface_ctx *curr = candidate;
    for (int depth = 0; curr && depth < 64; depth++) {
        if (curr == root) {
            return true;
        }
        curr = (curr->remote_as_child && curr->remote_as_child->attached)
                   ? curr->remote_as_child->parent
                   : NULL;
    }
    return false;
}

/* ==================== 场景图查找与挂载 ==================== */

static struct wlr_scene_buffer *
find_surface_scene_buffer_in_node(struct wlr_scene_node *node, struct wlr_surface *target)
{
    if (!node || !target) {
        return NULL;
    }
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
        if (ss && ss->surface == target) {
            return buf;
        }
        return NULL;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return NULL;
    }
    struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        struct wlr_scene_buffer *found = find_surface_scene_buffer_in_node(child, target);
        if (found) {
            return found;
        }
    }
    return NULL;
}

static struct wlr_scene_buffer *
find_surface_scene_buffer(struct woodland_server *server, struct wlr_surface *surface)
{
    if (!server || !server->scene || !surface) {
        return NULL;
    }
    return find_surface_scene_buffer_in_node(&server->scene->tree.node, surface);
}

static void handle_remote_scene_tree_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct remote_subsurface_ctx *remote =
        wl_container_of(listener, remote, scene_tree_destroy);
    if (remote->scene_tree_destroy.link.prev) {
        wl_list_remove(&remote->scene_tree_destroy.link);
        remote->scene_tree_destroy.link.prev = NULL;
    }
    remote->scene_tree = NULL;
}

static void remote_destroy_scene_tree(struct remote_subsurface_ctx *remote)
{
    if (!remote || !remote->scene_tree) {
        return;
    }
    struct wlr_scene_tree *tree = remote->scene_tree;
    remote->scene_tree = NULL;
    if (remote->scene_tree_destroy.link.prev) {
        wl_list_remove(&remote->scene_tree_destroy.link);
        remote->scene_tree_destroy.link.prev = NULL;
    }
    wlr_scene_node_destroy(&tree->node);
}

static void remote_ensure_scene_tree(struct remote_subsurface_ctx *remote)
{
    if (!remote || !remote->attached || remote->scene_tree) {
        return;
    }
    if (!remote->family || !remote->family->server || !remote->parent || !remote->child) {
        return;
    }
    struct wlr_surface *parent_surf = remote->parent->surface;
    struct wlr_surface *child_surf = remote->child->surface;
    if (!parent_surf || !child_surf) {
        return;
    }

    struct wlr_scene_buffer *parent_buf =
        find_surface_scene_buffer(remote->family->server, parent_surf);
    if (!parent_buf || !parent_buf->node.parent) {
        return;
    }

    struct wlr_scene_tree *parent_tree = parent_buf->node.parent;
    remote->scene_tree = wlr_scene_subsurface_tree_create(parent_tree, child_surf);
    if (!remote->scene_tree) {
        return;
    }

    woodland_listener_connect(&remote->scene_tree_destroy,
                              &remote->scene_tree->node.events.destroy,
                              handle_remote_scene_tree_destroy,
                              "remote_subsurface.scene_tree_destroy");
    wlr_scene_node_set_position(&remote->scene_tree->node, remote->x, remote->y);
    remote_sync_parent_scene_order(remote->parent);
}

static void remote_sync_parent_scene_order(struct exported_surface_ctx *parent)
{
    if (!parent || !parent->family || !parent->family->server || !parent->surface) {
        return;
    }
    struct wlr_scene_buffer *parent_buf =
        find_surface_scene_buffer(parent->family->server, parent->surface);
    if (!parent_buf || !parent_buf->node.parent) {
        return;
    }
    struct wlr_scene_tree *parent_tree = parent_buf->node.parent;

    /*
     * 顺序约定：
     *   below_children（从底到顶）排在父表面 buffer 之下；
     *   above_children（从底到顶）排在父表面树最上方。
     */
    struct wlr_scene_node *anchor = &parent_buf->node;
    struct remote_subsurface_ctx *remote;
    wl_list_for_each_reverse(remote, &parent->below_children, parent_link) {
        if (remote->scene_tree && remote->scene_tree->node.parent == parent_tree &&
            &remote->scene_tree->node != anchor) {
            wlr_scene_node_place_below(&remote->scene_tree->node, anchor);
            anchor = &remote->scene_tree->node;
        }
    }

    wl_list_for_each(remote, &parent->above_children, parent_link) {
        if (remote->scene_tree && remote->scene_tree->node.parent == parent_tree) {
            wlr_scene_node_raise_to_top(&remote->scene_tree->node);
        }
    }
}

/* ==================== wlroots 角色与级联 Map/Unmap ==================== */

static void remote_cascade_unmap(struct remote_subsurface_ctx *remote)
{
    if (!remote || !remote->child) {
        return;
    }
    struct wlr_surface *child_surf = remote->child->surface;
    if (child_surf && child_surf->mapped) {
        wlr_surface_unmap(child_surf);
    }

    struct remote_subsurface_ctx *grandchild;
    wl_list_for_each(grandchild, &remote->child->below_children, parent_link) {
        remote_cascade_unmap(grandchild);
    }
    wl_list_for_each(grandchild, &remote->child->above_children, parent_link) {
        remote_cascade_unmap(grandchild);
    }
}

static void remote_recheck_mapping(struct remote_subsurface_ctx *remote)
{
    if (!remote || !remote->attached || !remote->parent || !remote->child) {
        return;
    }
    struct wlr_surface *parent_surf = remote->parent->surface;
    struct wlr_surface *child_surf = remote->child->surface;
    if (!parent_surf || !child_surf) {
        return;
    }

    remote_ensure_scene_tree(remote);

    bool parent_mapped = parent_surf->mapped;
    bool child_has_buffer = wlr_surface_has_buffer(child_surf);
    bool should_map = parent_mapped && child_has_buffer;

    if (should_map && !child_surf->mapped) {
        wlr_surface_map(child_surf);
        remote_ensure_scene_tree(remote);

        struct remote_subsurface_ctx *grandchild;
        wl_list_for_each(grandchild, &remote->child->below_children, parent_link) {
            remote_recheck_mapping(grandchild);
        }
        wl_list_for_each(grandchild, &remote->child->above_children, parent_link) {
            remote_recheck_mapping(grandchild);
        }
    } else if (!should_map && child_surf->mapped) {
        remote_cascade_unmap(remote);
    }
}

/* 协议规定：The wl_surface.offset request is ignored; clients must use set_position instead. */
static void handle_child_client_commit(struct wl_listener *listener, void *data)
{
    (void)data;
    struct remote_subsurface_ctx *remote =
        wl_container_of(listener, remote, child_client_commit);
    if (!remote->attached || !remote->child || !remote->child->surface) {
        return;
    }
    struct wlr_surface *surface = remote->child->surface;
    surface->pending.committed &= ~(uint32_t)WLR_SURFACE_STATE_OFFSET;
    surface->pending.dx = 0;
    surface->pending.dy = 0;
}

static void remote_role_commit(struct wlr_surface *surface)
{
    if (!surface || !surface->role_resource) {
        return;
    }
    struct remote_subsurface_ctx *remote = wl_resource_get_user_data(surface->role_resource);
    if (!remote || !remote->attached) {
        return;
    }
    remote_recheck_mapping(remote);
}

static void remote_role_destroy(struct wlr_surface *surface)
{
    /* wlroots 在销毁 role_resource 时已调 wlr_surface_unmap；资源清理由
     * remote_resource_destroy 完成。 */
    (void)surface;
}

static const struct wlr_surface_role remote_subsurface_role = {
    .name = "treeland_remote_subsurface_v1",
    .no_object = false,
    .client_commit = NULL,
    .commit = remote_role_commit,
    .map = NULL,
    .unmap = NULL,
    .destroy = remote_role_destroy,
};

/* ==================== 父子关联管理 ==================== */

static void handle_parent_map_or_unmap(struct wl_listener *listener, void *data)
{
    (void)data;
    struct remote_subsurface_ctx *remote = wl_container_of(listener, remote, parent_map);
    remote_recheck_mapping(remote);
}

static void handle_parent_unmap(struct wl_listener *listener, void *data)
{
    (void)data;
    struct remote_subsurface_ctx *remote = wl_container_of(listener, remote, parent_unmap);
    remote_recheck_mapping(remote);
}

static void handle_parent_commit(struct wl_listener *listener, void *data)
{
    (void)data;
    struct remote_subsurface_ctx *remote = wl_container_of(listener, remote, parent_commit);
    if (!remote->attached) {
        return;
    }
    if (!remote->scene_tree) {
        remote_ensure_scene_tree(remote);
    } else if (remote->parent) {
        remote_sync_parent_scene_order(remote->parent);
    }
}

static void remote_detach(struct remote_subsurface_ctx *remote)
{
    if (!remote || !remote->attached) {
        return;
    }
    struct exported_surface_ctx *old_child = remote->child;
    struct exported_surface_ctx *old_parent = remote->parent;

    remote_cascade_unmap(remote);
    remote_destroy_scene_tree(remote);

    if (remote->parent_map.link.prev) {
        wl_list_remove(&remote->parent_map.link);
        remote->parent_map.link.prev = NULL;
    }
    if (remote->parent_unmap.link.prev) {
        wl_list_remove(&remote->parent_unmap.link);
        remote->parent_unmap.link.prev = NULL;
    }
    if (remote->parent_commit.link.prev) {
        wl_list_remove(&remote->parent_commit.link);
        remote->parent_commit.link.prev = NULL;
    }
    if (remote->child_client_commit.link.prev) {
        wl_list_remove(&remote->child_client_commit.link);
        remote->child_client_commit.link.prev = NULL;
    }
    if (remote->parent_link.prev) {
        wl_list_remove(&remote->parent_link);
        remote->parent_link.prev = NULL;
    }

    if (old_child) {
        if (old_child->surface) {
            woodland_remote_subsurface_set_parent(old_child->surface, NULL);
        }
        if (old_child->remote_as_child == remote) {
            old_child->remote_as_child = NULL;
        }
    }

    remote->child = NULL;
    remote->parent = NULL;
    remote->attached = false;

    if (old_child && old_child != old_parent) {
        exported_maybe_free(old_child);
    }
    if (old_parent) {
        exported_maybe_free(old_parent);
    }
}

static struct remote_subsurface_ctx *
find_sibling_remote(struct remote_subsurface_ctx *remote, const char *sibling_token)
{
    if (!remote || !remote->attached || !remote->parent || !remote->family) {
        return NULL;
    }
    struct exported_surface_ctx *sibling_exp =
        find_exported_by_token(remote->family, sibling_token);
    if (!sibling_exp) {
        return NULL;
    }
    struct remote_subsurface_ctx *cand;
    wl_list_for_each(cand, &remote->parent->below_children, parent_link) {
        if (cand->child == sibling_exp) {
            return cand;
        }
    }
    wl_list_for_each(cand, &remote->parent->above_children, parent_link) {
        if (cand->child == sibling_exp) {
            return cand;
        }
    }
    return NULL;
}

/* ==================== treeland_remote_subsurface_v1 请求 ==================== */

static void remote_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void remote_handle_set_position(struct wl_client *client,
                                       struct wl_resource *resource,
                                       int32_t x, int32_t y)
{
    (void)client;
    struct remote_subsurface_ctx *remote = wl_resource_get_user_data(resource);
    if (!remote || !remote->attached) {
        return;
    }
    remote->x = x;
    remote->y = y;
    if (remote->family) {
        remote->family->position_count++;
    }
    remote_ensure_scene_tree(remote);
    if (remote->scene_tree) {
        wlr_scene_node_set_position(&remote->scene_tree->node, x, y);
    }
    woodland_trace_add("remote_subsurface", 0, "set_position (%.8s) -> (%d,%d)",
                       remote->child ? remote->child->token : "-", x, y);
}

static void remote_handle_place_above(struct wl_client *client,
                                      struct wl_resource *resource,
                                      const char *sibling_token)
{
    (void)client;
    struct remote_subsurface_ctx *remote = wl_resource_get_user_data(resource);
    if (!remote || !remote->attached || !remote->parent || !remote->family) {
        return;
    }
    struct exported_surface_ctx *parent = remote->parent;
    if (!sibling_token || sibling_token[0] == '\0') {
        /* 置于所有兄弟的最顶部（above_children 尾部） */
        wl_list_remove(&remote->parent_link);
        wl_list_insert(parent->above_children.prev, &remote->parent_link);
        remote->in_below = false;
        remote->family->restack_count++;
        remote_sync_parent_scene_order(parent);
        woodland_trace_add("remote_subsurface", 0, "place_above top (%.8s)",
                           remote->child ? remote->child->token : "-");
        return;
    }

    struct exported_surface_ctx *sibling_exp =
        find_exported_by_token(remote->family, sibling_token);
    if (!sibling_exp) {
        remote->family->invalid_sibling_count++;
        treeland_remote_subsurface_v1_send_invalid_sibling(resource, sibling_token);
        woodland_trace_add("remote_subsurface", 0, "place_above invalid_sibling token=%.8s",
                           sibling_token);
        return;
    }

    if (sibling_exp == parent) {
        /* 紧贴父表面之上（above_children 头部） */
        wl_list_remove(&remote->parent_link);
        wl_list_insert(&parent->above_children, &remote->parent_link);
        remote->in_below = false;
        remote->family->restack_count++;
        remote_sync_parent_scene_order(parent);
        woodland_trace_add("remote_subsurface", 0, "place_above parent (%.8s)",
                           remote->child ? remote->child->token : "-");
        return;
    }

    struct remote_subsurface_ctx *sibling_remote = find_sibling_remote(remote, sibling_token);
    if (!sibling_remote) {
        remote->family->invalid_sibling_count++;
        treeland_remote_subsurface_v1_send_invalid_sibling(resource, sibling_token);
        woodland_trace_add("remote_subsurface", 0, "place_above non_sibling token=%.8s",
                           sibling_token);
        return;
    }

    if (sibling_remote != remote) {
        wl_list_remove(&remote->parent_link);
        /* 插入到 sibling_remote 之后（即更高 Z 序） */
        wl_list_insert(&sibling_remote->parent_link, &remote->parent_link);
        remote->in_below = sibling_remote->in_below;
    }
    remote->family->restack_count++;
    remote_sync_parent_scene_order(parent);
    woodland_trace_add("remote_subsurface", 0, "place_above sibling (%.8s -> %.8s)",
                       remote->child ? remote->child->token : "-", sibling_token);
}

static void remote_handle_place_below(struct wl_client *client,
                                      struct wl_resource *resource,
                                      const char *sibling_token)
{
    (void)client;
    struct remote_subsurface_ctx *remote = wl_resource_get_user_data(resource);
    if (!remote || !remote->attached || !remote->parent || !remote->family) {
        return;
    }
    struct exported_surface_ctx *parent = remote->parent;
    if (!sibling_token || sibling_token[0] == '\0') {
        /* 置于所有兄弟的最底部（below_children 头部） */
        wl_list_remove(&remote->parent_link);
        wl_list_insert(&parent->below_children, &remote->parent_link);
        remote->in_below = true;
        remote->family->restack_count++;
        remote_sync_parent_scene_order(parent);
        woodland_trace_add("remote_subsurface", 0, "place_below bottom (%.8s)",
                           remote->child ? remote->child->token : "-");
        return;
    }

    struct exported_surface_ctx *sibling_exp =
        find_exported_by_token(remote->family, sibling_token);
    if (!sibling_exp) {
        remote->family->invalid_sibling_count++;
        treeland_remote_subsurface_v1_send_invalid_sibling(resource, sibling_token);
        woodland_trace_add("remote_subsurface", 0, "place_below invalid_sibling token=%.8s",
                           sibling_token);
        return;
    }

    if (sibling_exp == parent) {
        /* 紧贴父表面之下（below_children 尾部） */
        wl_list_remove(&remote->parent_link);
        wl_list_insert(parent->below_children.prev, &remote->parent_link);
        remote->in_below = true;
        remote->family->restack_count++;
        remote_sync_parent_scene_order(parent);
        woodland_trace_add("remote_subsurface", 0, "place_below parent (%.8s)",
                           remote->child ? remote->child->token : "-");
        return;
    }

    struct remote_subsurface_ctx *sibling_remote = find_sibling_remote(remote, sibling_token);
    if (!sibling_remote) {
        remote->family->invalid_sibling_count++;
        treeland_remote_subsurface_v1_send_invalid_sibling(resource, sibling_token);
        woodland_trace_add("remote_subsurface", 0, "place_below non_sibling token=%.8s",
                           sibling_token);
        return;
    }

    if (sibling_remote != remote) {
        wl_list_remove(&remote->parent_link);
        /* 插入到 sibling_remote 之前（即更低 Z 序） */
        wl_list_insert(sibling_remote->parent_link.prev, &remote->parent_link);
        remote->in_below = sibling_remote->in_below;
    }
    remote->family->restack_count++;
    remote_sync_parent_scene_order(parent);
    woodland_trace_add("remote_subsurface", 0, "place_below sibling (%.8s -> %.8s)",
                       remote->child ? remote->child->token : "-", sibling_token);
}

static const struct treeland_remote_subsurface_v1_interface remote_subsurface_impl = {
    .destroy = remote_handle_destroy,
    .set_position = remote_handle_set_position,
    .place_above = remote_handle_place_above,
    .place_below = remote_handle_place_below,
};

static void remote_resource_destroy(struct wl_resource *resource)
{
    struct remote_subsurface_ctx *remote = wl_resource_get_user_data(resource);
    if (!remote) {
        return;
    }
    remote_detach(remote);
    if (remote->link.prev) {
        wl_list_remove(&remote->link);
        remote->link.prev = NULL;
    }
    free(remote);
}

/* 创建 inert 的 treeland_remote_subsurface_v1 资源（当 parent_rejected 时返回给客户端） */
static struct wl_resource *
create_inert_remote_resource(struct wl_client *client, uint32_t version, uint32_t id)
{
    struct wl_resource *res = wl_resource_create(
        client, &treeland_remote_subsurface_v1_interface, (int)version, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return NULL;
    }
    wl_resource_set_implementation(res, &remote_subsurface_impl, NULL, NULL);
    return res;
}

/* ==================== treeland_exported_surface_v1 请求 ==================== */

static bool exported_has_relations(const struct exported_surface_ctx *ctx)
{
    if (!ctx) {
        return false;
    }
    return ctx->remote_as_child != NULL ||
           !wl_list_empty(&ctx->below_children) ||
           !wl_list_empty(&ctx->above_children);
}

static void exported_maybe_free(struct exported_surface_ctx *ctx)
{
    if (!ctx || ctx->in_surface_destroy) {
        return;
    }
    if (ctx->resource != NULL || exported_has_relations(ctx)) {
        return;
    }
    if (ctx->surface_destroy.link.prev) {
        wl_list_remove(&ctx->surface_destroy.link);
        ctx->surface_destroy.link.prev = NULL;
    }
    ctx->surface = NULL;
    if (ctx->link.prev) {
        wl_list_remove(&ctx->link);
        ctx->link.prev = NULL;
    }
    free(ctx);
}

static void handle_exported_surface_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct exported_surface_ctx *ctx =
        wl_container_of(listener, ctx, surface_destroy);
    /*
     * 底层 wlr_surface 正在销毁：立即摘除 surface_destroy 与全部子关联的
     * parent_map/unmap/commit 监听器，避免 wlroots surface_handle_resource_destroy
     * 断言信号链表为空。用 in_surface_destroy 防止 remote_detach 内重入提前 free(ctx)。
     */
    ctx->in_surface_destroy = true;
    ctx->revoked = true;
    if (ctx->surface_destroy.link.prev) {
        wl_list_remove(&ctx->surface_destroy.link);
        ctx->surface_destroy.link.prev = NULL;
    }

    struct remote_subsurface_ctx *remote, *tmp;
    wl_list_for_each_safe(remote, tmp, &ctx->below_children, parent_link) {
        remote_detach(remote);
    }
    wl_list_for_each_safe(remote, tmp, &ctx->above_children, parent_link) {
        remote_detach(remote);
    }
    if (ctx->remote_as_child) {
        remote_detach(ctx->remote_as_child);
    }
    ctx->surface = NULL;
    ctx->in_surface_destroy = false;
    exported_maybe_free(ctx);
}

static void exported_resource_destroy(struct wl_resource *resource)
{
    struct exported_surface_ctx *ctx = wl_resource_get_user_data(resource);
    if (!ctx) {
        return;
    }
    /*
     * 协议 §treeland_exported_surface_v1：
     * "Destroying this object revokes the surface_token without affecting
     *  existing parent-child relationships or treeland_remote_subsurface_v1 objects."
     */
    ctx->resource = NULL;
    ctx->revoked = true;
    exported_maybe_free(ctx);
}

static void exported_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void exported_handle_create_remote_subsurface(struct wl_client *client,
                                                     struct wl_resource *resource,
                                                     uint32_t id,
                                                     const char *parent_token)
{
    struct exported_surface_ctx *child_ctx = wl_resource_get_user_data(resource);
    uint32_t version = (uint32_t)wl_resource_get_version(resource);
    if (!child_ctx || !child_ctx->family) {
        create_inert_remote_resource(client, version, id);
        return;
    }
    struct woodland_proto_remote_subsurface *family = child_ctx->family;

    if (!parent_token || parent_token[0] == '\0' || child_ctx->remote_as_child) {
        family->reject_count++;
        create_inert_remote_resource(client, version, id);
        treeland_exported_surface_v1_send_parent_rejected(
            resource,
            TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_INVALID_TOKEN,
            parent_token ? parent_token : "");
        woodland_trace_add("remote_subsurface", 0,
                           "rejected invalid_token (empty_or_duplicate) token=%.8s",
                           parent_token ? parent_token : "");
        return;
    }

    struct exported_surface_ctx *parent_ctx = find_exported_by_token(family, parent_token);
    if (!parent_ctx || !parent_ctx->surface) {
        family->reject_count++;
        create_inert_remote_resource(client, version, id);
        treeland_exported_surface_v1_send_parent_rejected(
            resource,
            TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_INVALID_TOKEN,
            parent_token);
        woodland_trace_add("remote_subsurface", 0, "rejected invalid_token token=%.8s",
                           parent_token);
        return;
    }

    if (parent_ctx == child_ctx || is_descendant_of(parent_ctx, child_ctx)) {
        family->reject_count++;
        create_inert_remote_resource(client, version, id);
        treeland_exported_surface_v1_send_parent_rejected(
            resource,
            TREELAND_EXPORTED_SURFACE_V1_REJECT_REASON_CYCLIC,
            parent_token);
        woodland_trace_add("remote_subsurface", 0, "rejected cyclic child=%.8s parent=%.8s",
                           child_ctx->token, parent_token);
        return;
    }

    struct wlr_surface *child_surf = child_ctx->surface;
    if (!child_surf) {
        create_inert_remote_resource(client, version, id);
        return;
    }

    if (!wlr_surface_set_role(child_surf, &remote_subsurface_role, resource,
                              TREELAND_EXPORTED_SURFACE_V1_ERROR_BAD_SURFACE)) {
        /* 角色冲突：wlr_surface_set_role 已发送 bad_surface 协议错误 */
        create_inert_remote_resource(client, version, id);
        return;
    }

    struct wl_resource *remote_res = wl_resource_create(
        client, &treeland_remote_subsurface_v1_interface, (int)version, id);
    if (!remote_res) {
        wl_client_post_no_memory(client);
        return;
    }

    struct remote_subsurface_ctx *remote = calloc(1, sizeof(*remote));
    if (!remote) {
        wl_resource_destroy(remote_res);
        wl_client_post_no_memory(client);
        return;
    }

    remote->family = family;
    remote->resource = remote_res;
    remote->child = child_ctx;
    remote->parent = parent_ctx;
    remote->in_below = false;
    remote->attached = true;
    remote->x = 0;
    remote->y = 0;

    wl_resource_set_implementation(remote_res, &remote_subsurface_impl, remote,
                                   remote_resource_destroy);
    wlr_surface_set_role_object(child_surf, remote_res);

    /* 默认加在 above_children 的最顶部 */
    wl_list_insert(parent_ctx->above_children.prev, &remote->parent_link);
    wl_list_insert(&family->remote_subsurfaces, &remote->link);
    child_ctx->remote_as_child = remote;
    woodland_remote_subsurface_set_parent(child_surf, parent_ctx->surface);

    woodland_listener_connect(&remote->parent_map, &parent_ctx->surface->events.map,
                              handle_parent_map_or_unmap, "remote_subsurface.parent_map");
    woodland_listener_connect(&remote->parent_unmap, &parent_ctx->surface->events.unmap,
                              handle_parent_unmap, "remote_subsurface.parent_unmap");
    woodland_listener_connect(&remote->parent_commit, &parent_ctx->surface->events.commit,
                              handle_parent_commit, "remote_subsurface.parent_commit");
    woodland_listener_connect(&remote->child_client_commit, &child_surf->events.client_commit,
                              handle_child_client_commit, "remote_subsurface.child_client_commit");

    family->create_count++;
    woodland_trace_add("remote_subsurface", 0, "created child=%.8s -> parent=%.8s",
                       child_ctx->token, parent_ctx->token);

    remote_recheck_mapping(remote);
}

static const struct treeland_exported_surface_v1_interface exported_surface_impl = {
    .destroy = exported_handle_destroy,
    .create_remote_subsurface = exported_handle_create_remote_subsurface,
};

/* ==================== treeland_remote_subsurface_manager_v1 请求 ==================== */

static bool client_allowed(struct woodland_server *server, struct wl_client *client)
{
    return !woodland_proto_security_context_is_sandboxed(server->proto, client);
}

static void manager_binding_destroy(struct wl_resource *resource)
{
    struct remote_manager_binding *binding = wl_resource_get_user_data(resource);
    if (!binding) {
        return;
    }
    wl_list_remove(&binding->link);
    free(binding);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static void manager_handle_export_surface(struct wl_client *client,
                                          struct wl_resource *resource,
                                          uint32_t id,
                                          struct wl_resource *surface_resource)
{
    struct remote_manager_binding *binding = wl_resource_get_user_data(resource);
    struct woodland_proto_remote_subsurface *family = binding ? binding->family : NULL;
    if (!family) {
        return;
    }

    if (!surface_resource ||
        wl_resource_get_interface(surface_resource) != &wl_surface_interface) {
        wl_resource_post_error(resource,
                               TREELAND_REMOTE_SUBSURFACE_MANAGER_V1_ERROR_BAD_SURFACE,
                               "Invalid wl_surface resource");
        return;
    }

    struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);
    if (!wlr_surface) {
        wl_resource_post_error(resource,
                               TREELAND_REMOTE_SUBSURFACE_MANAGER_V1_ERROR_BAD_SURFACE,
                               "Invalid wl_surface resource");
        return;
    }

    if (find_exported_by_surface(family, wlr_surface) != NULL) {
        wl_resource_post_error(
            resource,
            TREELAND_REMOTE_SUBSURFACE_MANAGER_V1_ERROR_BAD_SURFACE,
            "The wl_surface has already been exported through this manager");
        return;
    }

    uint32_t version = (uint32_t)wl_resource_get_version(resource);
    struct wl_resource *exp_res = wl_resource_create(
        client, &treeland_exported_surface_v1_interface, (int)version, id);
    if (!exp_res) {
        wl_client_post_no_memory(client);
        return;
    }

    struct exported_surface_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        wl_resource_destroy(exp_res);
        wl_client_post_no_memory(client);
        return;
    }

    ctx->family = family;
    ctx->resource = exp_res;
    ctx->surface = wlr_surface;
    wl_list_init(&ctx->below_children);
    wl_list_init(&ctx->above_children);
    generate_unique_token(family, ctx->token);

    wl_resource_set_implementation(exp_res, &exported_surface_impl, ctx,
                                   exported_resource_destroy);
    wl_list_insert(&family->exported_surfaces, &ctx->link);

    woodland_listener_connect(&ctx->surface_destroy, &wlr_surface->events.destroy,
                              handle_exported_surface_destroy,
                              "remote_subsurface.surface_destroy");

    family->export_count++;
    woodland_trace_add("remote_subsurface", 0, "export_surface token=%.8s", ctx->token);
    treeland_exported_surface_v1_send_surface_token(exp_res, ctx->token);
}

static const struct treeland_remote_subsurface_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .export_surface = manager_handle_export_surface,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_remote_subsurface *family = data;
    if (!client_allowed(family->server, client)) {
        wlr_log(WLR_ERROR, "remote-subsurface: denied bind from sandboxed client");
        wl_client_post_implementation_error(
            client,
            "treeland_remote_subsurface_manager_v1: bind rejected for a sandboxed client");
        return;
    }

    if (version > REMOTE_SUBSURFACE_VERSION) {
        version = REMOTE_SUBSURFACE_VERSION;
    }
    struct wl_resource *resource = wl_resource_create(
        client, &treeland_remote_subsurface_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct remote_manager_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    binding->family = family;
    binding->resource = resource;
    wl_resource_set_implementation(resource, &manager_impl, binding, manager_binding_destroy);
    wl_list_insert(&family->bindings, &binding->link);
}

/* ==================== 状态查询与生命周期 ==================== */

void woodland_protocol_remote_subsurface_status(struct woodland_server *server,
                                                struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);

    struct woodland_proto_remote_subsurface *family =
        (server && server->proto) ? server->proto->remote_subsurface : NULL;
    if (!family || !family->global) {
        woodland_json_obj_bool(&obj, "available", false);
        woodland_json_obj_close(&obj);
        return;
    }

    uint32_t active_exports = 0;
    struct exported_surface_ctx *exp;
    wl_list_for_each(exp, &family->exported_surfaces, link) {
        if (!exp->revoked) {
            active_exports++;
        }
    }

    uint32_t active_remotes = 0;
    struct remote_subsurface_ctx *remote;
    wl_list_for_each(remote, &family->remote_subsurfaces, link) {
        if (remote->attached) {
            active_remotes++;
        }
    }

    woodland_json_obj_bool(&obj, "available", true);
    woodland_json_obj_int(&obj, "bindings", (long long)wl_list_length(&family->bindings));
    woodland_json_obj_int(&obj, "active_exports", (long long)active_exports);
    woodland_json_obj_int(&obj, "active_subsurfaces", (long long)active_remotes);
    woodland_json_obj_int(&obj, "export_count", (long long)family->export_count);
    woodland_json_obj_int(&obj, "create_count", (long long)family->create_count);
    woodland_json_obj_int(&obj, "reject_count", (long long)family->reject_count);
    woodland_json_obj_int(&obj, "invalid_sibling_count",
                          (long long)family->invalid_sibling_count);
    woodland_json_obj_int(&obj, "restack_count", (long long)family->restack_count);
    woodland_json_obj_int(&obj, "position_count", (long long)family->position_count);

    woodland_json_obj_key(&obj, "subsurfaces");
    struct woodland_json_arr arr;
    woodland_json_arr_open(&arr, out);
    wl_list_for_each(remote, &family->remote_subsurfaces, link) {
        if (!remote->attached) {
            continue;
        }
        woodland_json_arr_sep(&arr);
        struct woodland_json_obj item;
        woodland_json_obj_open(&item, out);
        woodland_json_obj_str(&item, "place", remote->in_below ? "below" : "above");
        woodland_json_obj_int(&item, "x", remote->x);
        woodland_json_obj_int(&item, "y", remote->y);
        woodland_json_obj_bool(&item, "mapped",
                               remote->child && remote->child->surface &&
                                   remote->child->surface->mapped);
        woodland_json_obj_bool(&item, "scene_attached", remote->scene_tree != NULL);
        woodland_json_obj_close(&item);
    }
    woodland_json_arr_close(&arr);

    woodland_json_obj_close(&obj);
}

bool woodland_proto_remote_subsurface_init(struct woodland_proto *proto)
{
    struct woodland_proto_remote_subsurface *family = calloc(1, sizeof(*family));
    if (!family) {
        wlr_log(WLR_ERROR, "cannot allocate remote-subsurface protocol state");
        return false;
    }
    family->server = proto->server;
    wl_list_init(&family->bindings);
    wl_list_init(&family->exported_surfaces);
    wl_list_init(&family->remote_subsurfaces);
    proto->remote_subsurface = family;

    family->global = wl_global_create(proto->server->display,
                                      &treeland_remote_subsurface_manager_v1_interface,
                                      REMOTE_SUBSURFACE_VERSION, family, manager_bind);
    if (!family->global) {
        wlr_log(WLR_ERROR, "failed to create treeland-remote-subsurface-manager global");
        return false;
    }
    wlr_log(WLR_INFO, "treeland-remote-subsurface-manager-v1: global advertised");
    return true;
}

void woodland_proto_remote_subsurface_finish(struct woodland_proto *proto)
{
    struct woodland_proto_remote_subsurface *family = proto ? proto->remote_subsurface : NULL;
    if (!family) {
        return;
    }
    if (family->global) {
        wl_global_destroy(family->global);
        family->global = NULL;
    }

    struct remote_subsurface_ctx *remote, *rtmp;
    wl_list_for_each_safe(remote, rtmp, &family->remote_subsurfaces, link) {
        wl_resource_destroy(remote->resource);
    }

    struct exported_surface_ctx *exp, *etmp;
    wl_list_for_each_safe(exp, etmp, &family->exported_surfaces, link) {
        if (exp->resource) {
            wl_resource_destroy(exp->resource);
        } else {
            exp->revoked = true;
            exported_maybe_free(exp);
        }
    }

    struct remote_manager_binding *binding, *btmp;
    wl_list_for_each_safe(binding, btmp, &family->bindings, link) {
        wl_resource_destroy(binding->resource);
    }

    free(family);
    proto->remote_subsurface = NULL;
}
