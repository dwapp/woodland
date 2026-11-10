// SPDX-License-Identifier: GPL-2.0-only
/*
 * wp-security-context-v1 实现（参考 labwc）。
 *
 * 边界说明：
 *   - 限制按「带有 security context 的 Wayland 连接」生效，而非按进程：
 *     Xwayland 下的 X11 应用、以及沙盒内直接连入主 WAYLAND_DISPLAY 的进程
 *     不经 create_listener + commit 建立私有连接，均不受限。
 *
 * 核心逻辑：
 *   1. 使用 wlroots 的 wlr_security_context_manager_v1 创建全局接口；
 *   2. 向统一全局过滤器（util/global_filter.h）注册 checker：
 *      - 禁止沙箱客户端访问 wp_security_context_manager_v1 本身（防嵌套沙箱与提权）；
 *      - 基于白名单机制（allowlist，对齐 labwc）向沙箱客户端放行安全、无特权的基础协议；
 *      - zwp_keyboard_shortcuts_inhibit_manager_v1 默认放行（支持沙箱全屏游戏/远程桌面）；
 *      - 严格屏蔽截屏、剪贴板监控、图层外壳、锁屏、输出配置、Xwayland shell 与各类
 *        系统级管理接口；
 *   3. 监听 manager->events.commit 与 manager->events.destroy 信号，记录沙箱绑定元数据、
 *      下发 trace，并在异常跳过 protocol_finish() 时兜底清理监听器；
 *   4. 提供视图与客户端层面的安全上下文反查，供 wctl 与诊断接口使用。
 *
 * 维护约定：
 *   本文件为 deny-by-default。在 woodland 中新增任何暴露给客户端的 wl_global 时，
 *   必须同步更新下面的 allowed_protocols 或 privileged_protocols 分类表（由单元测试
 *   test_security_context 校验所有真实暴露的 globals 均已被覆盖，未分类会导致 ERROR 日志）。
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/util/log.h>

#include "protocol.h"
#include "protocol_internal.h"
#include "trace.h"
#include "util/global_filter.h"
#include "util/json.h"
#include "util/listener.h"
#include "view.h"
#include "woodland.h"

#define SECURITY_CONTEXT_FILTER_NAME "security_context"

struct woodland_proto_security_context {
    struct woodland_proto *owner;
    struct woodland_server *server;
    struct wlr_security_context_manager_v1 *manager;
    struct wl_listener commit;
    struct wl_listener destroy;

    uint32_t commit_count;
    char *last_engine;
    char *last_app_id;
    char *last_instance_id;
};

/*
 * 允许沙箱客户端访问的 Wayland 协议白名单（与 woodland 实际暴露的 globals 对齐）。
 *
 * 参照 labwc 设计：默认拒绝（deny-by-default），只放行标准窗口绘制、
 * 输入响应与无特权装饰相关的协议，以及产品明确放行的快捷键抑制协议
 * （zwp_keyboard_shortcuts_inhibit_manager_v1）。
 */
static const char *const allowed_protocols[] = {
    /* 基础协议 */
    "wl_shm",
    "wl_compositor",
    "wl_subcompositor",
    "wl_data_device_manager",
    "wl_seat",
    "xdg_wm_base",
    "wl_fixes",

    /* 输出与渲染 */
    "wl_output",
    "zwp_linux_dmabuf_v1",
    "wp_presentation",
    "wp_viewporter",
    "wp_single_pixel_buffer_manager_v1",
    "wp_fractional_scale_manager_v1",
    "wp_tearing_control_manager_v1",
    "wp_alpha_modifier_v1",
    "wp_linux_drm_syncobj_manager_v1",
    "zxdg_output_manager_v1",

    /* 输入、选区与全屏快捷键抑制 */
    "zwp_primary_selection_device_manager_v1",
    "zwp_text_input_manager_v3",
    "zwp_pointer_gestures_v1",
    "wp_cursor_shape_manager_v1",
    "zwp_relative_pointer_manager_v1",
    "zwp_pointer_constraints_v1",
    "zwp_keyboard_shortcuts_inhibit_manager_v1",

    /* 窗口管理、装饰与对话框 */
    "xdg_activation_v1",
    "zxdg_decoration_manager_v1",
    "xdg_wm_dialog_v1",
    "xdg_toplevel_tag_manager_v1",
    "zwp_idle_inhibit_manager_v1",
    "treeland_decoration_manager_v1",
    "ext_background_effect_manager_v1",
    "treeland_appearance_v1",
};

/*
 * 特权协议黑名单（与 woodland 实际暴露的特权 globals 对齐，用于分类诊断与测试校验）。
 */
static const char *const privileged_protocols[] = {
    "wp_drm_lease_device_v1",
    "zwlr_gamma_control_manager_v1",
    "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1",
    "zwp_input_method_manager_v2",
    "zwp_virtual_keyboard_manager_v1",
    "zwlr_screencopy_manager_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "zwlr_data_control_manager_v1",
    "ext_data_control_manager_v1",
    "wp_security_context_manager_v1",
    "ext_idle_notifier_v1",
    "ext_workspace_manager_v1",
    "zwlr_foreign_toplevel_manager_v1",
    "ext_foreign_toplevel_list_v1",
    "ext_session_lock_manager_v1",
    "zwlr_layer_shell_v1",
    "xwayland_shell_v1",
    "treeland_wallpaper_manager_v1",
    "treeland_wallpaper_shell_v1",
    "treeland_wallpaper_notifier_v1",
    "treeland_compositor_action_v1",
    "treeland_shortcut_manager_v3",
    "treeland_foreign_toplevel_manager_v3",
    "treeland_appearance_manager_v1",
};

bool woodland_security_context_is_allowed(const char *interface_name)
{
    if (!interface_name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed_protocols) / sizeof(allowed_protocols[0]); i++) {
        if (strcmp(interface_name, allowed_protocols[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool woodland_security_context_is_privileged(const char *interface_name)
{
    if (!interface_name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(privileged_protocols) / sizeof(privileged_protocols[0]); i++) {
        if (strcmp(interface_name, privileged_protocols[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool sec_ctx_global_filter(const struct wl_client *client,
                                  const struct wl_global *global,
                                  void *data)
{
    struct woodland_proto_security_context *sec = data;
    if (!sec || !sec->manager) {
        return true;
    }

    const struct wl_interface *iface = wl_global_get_interface(global);
    if (!iface || !iface->name) {
        return false;
    }

    const struct wlr_security_context_v1_state *sec_ctx =
        wlr_security_context_manager_v1_lookup_client(sec->manager, (struct wl_client *)client);
    if (!sec_ctx) {
        return true;
    }

    /* 禁止沙箱客户端访问 wp_security_context_manager_v1 本身（防止嵌套沙箱与提权） */
    bool allow = (global != sec->manager->global) &&
                 woodland_security_context_is_allowed(iface->name);
    if (!allow) {
        if (!woodland_security_context_is_privileged(iface->name)) {
            wlr_log(WLR_ERROR, "Blocking unknown protocol %s for security context", iface->name);
        } else {
            wlr_log(WLR_DEBUG, "Blocking %s for security context %s->%s->%s",
                    iface->name,
                    sec_ctx->sandbox_engine ? sec_ctx->sandbox_engine : "(null)",
                    sec_ctx->app_id ? sec_ctx->app_id : "(null)",
                    sec_ctx->instance_id ? sec_ctx->instance_id : "(null)");
        }
    }
    return allow;
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct woodland_proto_security_context *sec =
        wl_container_of(listener, sec, commit);
    struct wlr_security_context_v1_commit_event *event = data;

    sec->commit_count++;

    char *engine = NULL;
    char *app_id = NULL;
    char *instance_id = NULL;
    bool oom = false;

    if (event->state->sandbox_engine) {
        engine = strdup(event->state->sandbox_engine);
        if (!engine) {
            oom = true;
        }
    }
    if (!oom && event->state->app_id) {
        app_id = strdup(event->state->app_id);
        if (!app_id) {
            oom = true;
        }
    }
    if (!oom && event->state->instance_id) {
        instance_id = strdup(event->state->instance_id);
        if (!instance_id) {
            oom = true;
        }
    }

    if (oom) {
        free(engine);
        free(app_id);
        free(instance_id);
        wlr_log(WLR_ERROR, "security-context: failed to copy committed metadata (OOM)");
    } else {
        free(sec->last_engine);
        free(sec->last_app_id);
        free(sec->last_instance_id);
        sec->last_engine = engine;
        sec->last_app_id = app_id;
        sec->last_instance_id = instance_id;
    }

    wlr_log(WLR_INFO, "security-context: committed engine='%s', app_id='%s', instance_id='%s'",
            event->state->sandbox_engine ? event->state->sandbox_engine : "(null)",
            event->state->app_id ? event->state->app_id : "(null)",
            event->state->instance_id ? event->state->instance_id : "(null)");

    woodland_trace_add("sec_ctx", 0, "commit engine=%s app_id=%s instance_id=%s",
                       event->state->sandbox_engine ? event->state->sandbox_engine : "(null)",
                       event->state->app_id ? event->state->app_id : "(null)",
                       event->state->instance_id ? event->state->instance_id : "(null)");
}

static void security_context_cleanup(struct woodland_proto_security_context *sec,
                                     bool remove_global_filter)
{
    if (!sec) {
        return;
    }
    if (remove_global_filter && sec->server && sec->server->display) {
        woodland_global_filter_remove(sec->server->display, SECURITY_CONTEXT_FILTER_NAME);
    }
    if (sec->commit.link.prev) {
        wl_list_remove(&sec->commit.link);
        sec->commit.link.prev = NULL;
    }
    if (sec->destroy.link.prev) {
        wl_list_remove(&sec->destroy.link);
        sec->destroy.link.prev = NULL;
    }
    sec->manager = NULL;
    if (sec->owner && sec->owner->security_context == sec) {
        sec->owner->security_context = NULL;
    }
    free(sec->last_engine);
    free(sec->last_app_id);
    free(sec->last_instance_id);
    free(sec);
}

/*
 * 异常路径兜底：wlroots 在 wl_display_destroy -> handle_display_destroy() 中
 * 依次发射 manager->events.destroy，随后断言 destroy/commit 监听器链表为空。
 * 若调用方跳过 woodland_protocol_finish() 直接销毁 display，在此摘除全部监听器。
 */
static void handle_manager_destroy(struct wl_listener *listener, void *data)
{
    (void)data;
    struct woodland_proto_security_context *sec =
        wl_container_of(listener, sec, destroy);
    security_context_cleanup(sec, true);
}

bool woodland_proto_security_context_init(struct woodland_proto *proto)
{
    if (!proto || !proto->server || !proto->server->display) {
        return false;
    }

    struct woodland_proto_security_context *sec = calloc(1, sizeof(*sec));
    if (!sec) {
        wlr_log(WLR_ERROR, "cannot allocate security context state");
        return false;
    }
    sec->owner = proto;
    sec->server = proto->server;

    sec->manager = wlr_security_context_manager_v1_create(proto->server->display);
    if (!sec->manager) {
        wlr_log(WLR_ERROR, "failed to create wlr_security_context_manager_v1");
        free(sec);
        return false;
    }

    woodland_listener_connect(&sec->commit, &sec->manager->events.commit,
                              handle_commit, "security_context.commit");
    woodland_listener_connect(&sec->destroy, &sec->manager->events.destroy,
                              handle_manager_destroy, "security_context.destroy");

    if (!woodland_global_filter_add(proto->server->display, SECURITY_CONTEXT_FILTER_NAME,
                                    sec_ctx_global_filter, sec)) {
        wlr_log(WLR_ERROR, "failed to register security context global filter");
        security_context_cleanup(sec, false);
        return false;
    }

    proto->security_context = sec;
    wlr_log(WLR_INFO, "wp_security_context_manager_v1 enabled");
    return true;
}

void woodland_proto_security_context_finish(struct woodland_proto *proto)
{
    if (!proto || !proto->security_context) {
        return;
    }
    security_context_cleanup(proto->security_context, true);
}

const struct wlr_security_context_v1_state *
woodland_proto_security_context_lookup_client(struct woodland_proto *proto,
                                              const struct wl_client *client)
{
    if (!proto || !proto->security_context || !proto->security_context->manager || !client) {
        return NULL;
    }
    return wlr_security_context_manager_v1_lookup_client(proto->security_context->manager,
                                                         (struct wl_client *)client);
}

const struct wlr_security_context_v1_state *
woodland_protocol_client_security_context(struct woodland_server *server,
                                          const struct wl_client *client)
{
    if (!server || !server->proto) {
        return NULL;
    }
    return woodland_proto_security_context_lookup_client(server->proto, client);
}

const struct wlr_security_context_v1_state *
woodland_protocol_view_security_context(struct woodland_server *server,
                                        const struct woodland_view *view)
{
    if (!server || !server->proto || !view) {
        return NULL;
    }
    struct wlr_surface *surface = woodland_view_surface(view);
    if (!surface || !surface->resource) {
        return NULL;
    }
    struct wl_client *client = wl_resource_get_client(surface->resource);
    return woodland_protocol_client_security_context(server, client);
}

void woodland_protocol_security_context_status(struct woodland_server *server,
                                               struct woodland_json *out)
{
    struct woodland_json_obj obj;
    woodland_json_obj_open(&obj, out);

    struct woodland_proto_security_context *sec =
        (server && server->proto) ? server->proto->security_context : NULL;
    if (!sec || !sec->manager) {
        woodland_json_obj_bool(&obj, "available", false);
        woodland_json_obj_close(&obj);
        return;
    }

    woodland_json_obj_bool(&obj, "available", true);
    woodland_json_obj_int(&obj, "commit_count", (long long)sec->commit_count);
    woodland_json_obj_str(&obj, "last_engine", sec->last_engine);
    woodland_json_obj_str(&obj, "last_app_id", sec->last_app_id);
    woodland_json_obj_str(&obj, "last_instance_id", sec->last_instance_id);

    woodland_json_obj_close(&obj);
}
