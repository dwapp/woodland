// SPDX-License-Identifier: GPL-2.0-only
/*
 * treeland-compositor-action-unstable-v1：让 shell 客户端直接触发合成器动作。
 *
 * 协议是无状态的一次性请求：一个 global、destroy / trigger 两个请求，没有
 * event，也没有错误枚举。原文要求"做不到的动作应忽略、客户端不得假定已经
 * 生效"，所以这里按能力分档，并且**不装作生效**：
 *
 *   真做：  workspace_1..12 / prev_workspace / next_workspace
 *           -> woodland_workspace_switch()。woodland 只有 10 个工作区，
 *           动作 11/12 找不到目标，按"不能执行"忽略。前后切换在边界上
 *           也不绕回：没有上一个/下一个就是不能执行。
 *   置空：  15..30（show_desktop、多任务、缩放、FPS、锁屏、关机菜单的
 *           各个默认焦点变体、用户切换）。woodland 没有这些 shell 状态机，
 *           先只记 trace / 日志 / wctl，不改任何状态，也不发事件——协议
 *           本来就不允许对这些请求回事件。
 *   忽略：  枚举外的值（0、>30），静默忽略，但 wctl 里看得到。
 *
 * 权限：协议要求"必须拒绝非特权客户端的 bind"。woodland 是单用户 session
 * 合成器，没有 DDE 的 privilege 服务；这里只校验对端 uid 与合成器相同，
 * 挡住跨用户连接，同 uid 一律视为 session 内的 shell。**这不是 DDE 的特权
 * 判定**：按 app_id / 签名 / policy 授权得先有那套机制，属于单独一版；
 * 在此之前不假装实现了它（同 AGENTS.md 对 security-context 的取舍）。
 *
 * 生命周期：一个 bind 一个 wl_resource，没有子对象、没有 surface 监听器；
 * 但需要一份 binding 链表——global 销毁并不会带走已 bind 的 resource，
 * 而 resource 的 user_data 指向本族状态。finish 里必须先把它们全部销毁，
 * 否则本族状态 free 之后，活着的 resource 再收到 trigger 就是 UAF。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "protocol.h"
#include "protocol_internal.h"
#include "trace.h"
#include "workspace.h"
#include "woodland.h"

#include "treeland-compositor-action-unstable-v1-protocol.h"

/* 协议是 experimental：版本号写死 1，上游升版要显式跟 */
#define ACTION_VERSION 1

/* 值域用生成头里的常量钉住：上游不升版本号改枚举时，这里会跟着编译失败 */
#define ACTION_MIN TREELAND_COMPOSITOR_ACTION_V1_ACTION_WORKSPACE_1
#define ACTION_WORKSPACE_MAX TREELAND_COMPOSITOR_ACTION_V1_ACTION_WORKSPACE_12
#define ACTION_UNSUPPORTED_MIN TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHOW_DESKTOP
#define ACTION_MAX TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU_LOG_OUT

enum action_outcome {
    OUTCOME_NONE = 0,
    OUTCOME_SWITCHED,
    OUTCOME_NO_WORKSPACE,
    OUTCOME_UNSUPPORTED,
    OUTCOME_INVALID,
};

/* wctl 与 trace 共用的稳定字符串（冒烟脚本按名字断言，别写成一句人话） */
static const char *const outcome_names[] = {
    [OUTCOME_NONE] = "none",
    [OUTCOME_SWITCHED] = "switched",
    [OUTCOME_NO_WORKSPACE] = "no_workspace",
    [OUTCOME_UNSUPPORTED] = "unsupported",
    [OUTCOME_INVALID] = "invalid",
};

/* 置空动作的名字：只给 trace / 日志说明"忽略了哪一个"，不参与分派 */
static const char *const unsupported_names[] = {
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHOW_DESKTOP] = "show_desktop",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_OPEN_MULTITASK_VIEW] = "open_multitask_view",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_CLOSE_MULTITASK_VIEW] = "close_multitask_view",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_TOGGLE_MULTITASK_VIEW] = "toggle_multitask_view",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_ZOOM_IN] = "zoom_in",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_ZOOM_OUT] = "zoom_out",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_ZOOM_RESET] = "zoom_reset",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_TOGGLE_FPS_DISPLAY] = "toggle_fps_display",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_LOCKSCREEN] = "lockscreen",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU] = "shutdown_menu",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHOW_USER_SWITCH] = "show_user_switch",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU_POWER_OFF] = "shutdown_menu_power_off",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU_REBOOT] = "shutdown_menu_reboot",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU_SUSPEND] = "shutdown_menu_suspend",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU_HIBERNATE] = "shutdown_menu_hibernate",
    [TREELAND_COMPOSITOR_ACTION_V1_ACTION_SHUTDOWN_MENU_LOG_OUT] = "shutdown_menu_log_out",
};

/* 一个 bind 出来的对象；链表只是为了 finish 时能全部回收 */
struct action_binding {
    struct woodland_proto_compositor_action *act;
    struct wl_resource *resource;
    struct wl_list link;
};

struct woodland_proto_compositor_action {
    struct woodland_server *server;
    struct wl_global *global;
    struct wl_list bindings; // struct action_binding.link

    /* 只读诊断状态：协议本身无状态，这些只是"最近一次请求怎么处置的" */
    uint32_t trigger_count;
    uint32_t last_action;
    enum action_outcome last_outcome;
};

/* ==================== 纯逻辑（单元测试直接调） ==================== */

enum woodland_protocol_action_class
woodland_protocol_action_classify(uint32_t action)
{
    if (action >= ACTION_MIN && action <= ACTION_WORKSPACE_MAX) {
        return WOODLAND_PROTO_ACTION_WORKSPACE;
    }
    if (action == TREELAND_COMPOSITOR_ACTION_V1_ACTION_PREV_WORKSPACE) {
        return WOODLAND_PROTO_ACTION_WORKSPACE_PREV;
    }
    if (action == TREELAND_COMPOSITOR_ACTION_V1_ACTION_NEXT_WORKSPACE) {
        return WOODLAND_PROTO_ACTION_WORKSPACE_NEXT;
    }
    if (action >= ACTION_UNSUPPORTED_MIN && action <= ACTION_MAX) {
        return WOODLAND_PROTO_ACTION_UNSUPPORTED;
    }
    return WOODLAND_PROTO_ACTION_INVALID;
}

bool woodland_protocol_action_workspace_name(uint32_t action, char *buf, size_t len)
{
    /* 协议把 workspace_N 的值直接定成 N，所以值就是工作区名 */
    if (!buf || len == 0 ||
        woodland_protocol_action_classify(action) != WOODLAND_PROTO_ACTION_WORKSPACE) {
        return false;
    }
    int written = snprintf(buf, len, "%u", action);
    return written >= 0 && (size_t)written < len;
}

struct woodland_workspace *
woodland_protocol_action_adjacent_workspace(struct woodland_server *server, bool next)
{
    if (!server || !server->active_workspace ||
        wl_list_empty(&server->workspaces)) {
        return NULL;
    }
    struct wl_list *link = next ? server->active_workspace->link.next
                               : server->active_workspace->link.prev;
    /*
     * wl_list 是环形的：回到表头说明越过了边界。这里不绕回——协议要求
     * "不能执行的动作应忽略"，在第一个/最后一个工作区时没有 prev/next。
     */
    if (link == &server->workspaces) {
        return NULL;
    }
    struct woodland_workspace *ws = NULL;
    return wl_container_of(link, ws, link);
}

void woodland_protocol_compositor_action_status(struct woodland_server *server,
                                                bool *available,
                                                uint32_t *trigger_count,
                                                uint32_t *last_action,
                                                const char **last_outcome)
{
    struct woodland_proto_compositor_action *act =
        (server && server->proto) ? server->proto->compositor_action : NULL;
    if (available) {
        *available = act != NULL;
    }
    if (trigger_count) {
        *trigger_count = act ? act->trigger_count : 0;
    }
    if (last_action) {
        *last_action = act ? act->last_action : 0;
    }
    if (last_outcome) {
        *last_outcome = act ? outcome_names[act->last_outcome] : NULL;
    }
}

/* ==================== bind / 请求 ==================== */

/*
 * 最小权限校验：挡住跨用户连接。libwayland 在部分 fd 上取不到
 * SO_PEERCRED（uid == (uid_t)-1），这时按同 session 放行并留一条日志——
 * 没有别的判定手段，而误杀会连累同一连接上的其它 global（Xwayland、
 * in-process 客户端都走这条）。
 */
static bool client_allowed(struct wl_client *client, uid_t *out_uid)
{
    pid_t pid = 0;
    uid_t uid = (uid_t)-1;
    gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    (void)pid;
    (void)gid;
    if (out_uid) {
        *out_uid = uid;
    }
    if (uid == (uid_t)-1) {
        wlr_log(WLR_INFO, "compositor-action: no peer credentials, allowing bind");
        return true;
    }
    return uid == getuid();
}

static void binding_resource_destroy(struct wl_resource *resource)
{
    struct action_binding *binding = wl_resource_get_user_data(resource);
    if (!binding) {
        return;
    }
    wl_list_remove(&binding->link);
    free(binding);
}

static void action_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    /* 协议原文：destroy 不改变任何合成器 / shell / session 状态 */
    wl_resource_destroy(resource);
}

static void action_handle_trigger(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t action)
{
    (void)client;
    struct action_binding *binding = wl_resource_get_user_data(resource);
    if (!binding || !binding->act) {
        return;
    }
    struct woodland_proto_compositor_action *act = binding->act;
    struct woodland_server *server = act->server;
    enum woodland_protocol_action_class cls = woodland_protocol_action_classify(action);
    enum action_outcome outcome = OUTCOME_INVALID;
    char detail[128] = "";

    act->trigger_count++;
    act->last_action = action;

    switch (cls) {
    case WOODLAND_PROTO_ACTION_WORKSPACE: {
        char name[16] = "";
        if (!woodland_protocol_action_workspace_name(action, name, sizeof(name))) {
            outcome = OUTCOME_INVALID;
            snprintf(detail, sizeof(detail), "cannot format workspace name");
            break;
        }
        struct woodland_workspace *target = woodland_workspace_by_name(server, name);
        if (!target) {
            outcome = OUTCOME_NO_WORKSPACE;
            snprintf(detail, sizeof(detail), "workspace %s does not exist", name);
            break;
        }
        woodland_workspace_switch(server, target);
        outcome = OUTCOME_SWITCHED;
        snprintf(detail, sizeof(detail), "workspace %s", name);
        break;
    }
    case WOODLAND_PROTO_ACTION_WORKSPACE_PREV:
    case WOODLAND_PROTO_ACTION_WORKSPACE_NEXT: {
        bool next = cls == WOODLAND_PROTO_ACTION_WORKSPACE_NEXT;
        struct woodland_workspace *target =
            woodland_protocol_action_adjacent_workspace(server, next);
        if (!target) {
            outcome = OUTCOME_NO_WORKSPACE;
            snprintf(detail, sizeof(detail), "no %s workspace", next ? "next" : "previous");
            break;
        }
        woodland_workspace_switch(server, target);
        outcome = OUTCOME_SWITCHED;
        snprintf(detail, sizeof(detail), "%s workspace %s", next ? "next" : "previous",
                 target->name);
        break;
    }
    case WOODLAND_PROTO_ACTION_UNSUPPORTED: {
        const char *name = action <= ACTION_MAX ? unsupported_names[action] : NULL;
        outcome = OUTCOME_UNSUPPORTED;
        snprintf(detail, sizeof(detail), "not implemented: %s", name ? name : "unknown");
        break;
    }
    case WOODLAND_PROTO_ACTION_INVALID:
    default:
        outcome = OUTCOME_INVALID;
        snprintf(detail, sizeof(detail), "not in action enum");
        break;
    }

    act->last_outcome = outcome;
    /* detail 只写决策依据（走了哪个分支、为什么忽略），状态数值由 get_tree 查 */
    woodland_trace_add("action", 0, "value=%u %s -> %s", action, detail,
                       outcome_names[outcome]);
    wlr_log(WLR_INFO, "compositor-action: value=%u %s -> %s", action, detail,
            outcome_names[outcome]);
}

static const struct treeland_compositor_action_v1_interface action_impl = {
    .destroy = action_handle_destroy,
    .trigger = action_handle_trigger,
};

static void action_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct woodland_proto_compositor_action *act = data;
    uid_t uid = (uid_t)-1;
    if (!client_allowed(client, &uid)) {
        wlr_log(WLR_ERROR, "compositor-action: denied bind from uid %u (compositor uid %u)",
                (unsigned)uid, (unsigned)getuid());
        /* 协议没有自己的 error enum：用 wl_display 的 implementation 错误
         * 明确拒绝这次 bind，客户端拿不到对象，也不会把哑对象当成成功。 */
        wl_client_post_implementation_error(client,
            "treeland_compositor_action_v1: bind rejected for a different user");
        return;
    }

    /* 客户端不该请求比 global 更高的版本；按 global 的版本夹一下，免得
     * 把未定义的高版本请求当成"已支持"放进来 */
    if (version > ACTION_VERSION) {
        version = ACTION_VERSION;
    }
    struct wl_resource *resource = wl_resource_create(
        client, &treeland_compositor_action_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    struct action_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    binding->act = act;
    binding->resource = resource;
    /* 先挂析构再入链：后面无论客户端 destroy 还是 finish 回收都走同一条路 */
    wl_resource_set_implementation(resource, &action_impl, binding,
                                    binding_resource_destroy);
    wl_list_insert(&act->bindings, &binding->link);
}

/* ==================== 由编排层调用的入口 ==================== */

bool woodland_proto_compositor_action_init(struct woodland_proto *proto)
{
    struct woodland_proto_compositor_action *act = calloc(1, sizeof(*act));
    if (!act) {
        wlr_log(WLR_ERROR, "cannot allocate compositor-action protocol state");
        return false;
    }
    act->server = proto->server;
    wl_list_init(&act->bindings);
    /* 先挂上去：init 中途失败时 finish 也能把它收走 */
    proto->compositor_action = act;

    act->global = wl_global_create(proto->server->display,
                                    &treeland_compositor_action_v1_interface,
                                    ACTION_VERSION, act, action_bind);
    if (!act->global) {
        wlr_log(WLR_ERROR, "failed to create treeland-compositor-action global");
        return false;
    }
    wlr_log(WLR_INFO,
            "treeland-compositor-action-v1: workspace actions wired, 15-30 ignored");
    return true;
}

void woodland_proto_compositor_action_finish(struct woodland_proto *proto)
{
    struct woodland_proto_compositor_action *act = proto->compositor_action;
    if (!act) {
        return;
    }
    if (act->global) {
        wl_global_destroy(act->global);
        act->global = NULL;
    }
    /*
     * global 销毁不会带走已 bind 的 resource。先把它们全部销毁（destructor
     * 会摘链并释放 binding），再 free 本族状态；否则活着的 resource 上还
     * 留着指向 act 的 user_data，trigger 一到就是 UAF。
     */
    struct action_binding *binding, *tmp;
    wl_list_for_each_safe(binding, tmp, &act->bindings, link)
    {
        wl_resource_destroy(binding->resource);
    }
    free(act);
    proto->compositor_action = NULL;
}
