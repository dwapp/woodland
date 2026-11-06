// SPDX-License-Identifier: GPL-2.0-only
#include "util/listener.h"
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "capture.h"
#include "config.h"
#if WOODLAND_HAVE_EFFECTS
#include <scenefx/render/fx_renderer/fx_renderer.h>
#endif
#include "cursor.h"
#include "effect.h"
#include "ime.h"
#include "ipc.h"
#include "layer.h"
#include "output.h"
#include "protocol.h"
#include "seat.h"
#include "selfcheck.h"
#include "server.h"
#include "session_lock.h"
#include "view.h"
#include "woodland.h"
#include "woodland_config.h"
#include "workspace.h"
#if WOODLAND_HAVE_XWAYLAND
#include "xwayland.h"
#endif

static int handle_terminate(int signal, void *data)
{
    struct wl_display *display = data;
    wlr_log(WLR_INFO, "received signal %d, terminating", signal);
    wl_display_terminate(display);
    return 0;
}

static int handle_sighup(int signal, void *data)
{
    struct woodland_server *server = data;
    wlr_log(WLR_INFO, "received signal %d (SIGHUP), reloading configuration", signal);
    woodland_config_reload(server);
    return 0;
}

static void handle_request_set_shape(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, request_set_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    const char *name = wlr_cursor_shape_v1_name(event->shape);
    woodland_cursor_set(server, name);
}

/*
 * 新输出的第二次配置广播。
 *
 * 第一次是 wlr_output_layout_add_auto() 内部触发的（layout change → 协议层
 * 广播），那一刻输出刚进链表、还没 commit；而 headless 输出没有 preferred
 * mode，commit 只设 ENABLED，不会引起布局重算，于是再也没有信号来纠正那
 * 一份——wdisplays / kanshi 从此看不到这个输出。本监听器注册在
 * handle_new_output 之后，跑的时候输出已经完全就位。
 */
static void handle_new_output_broadcast(struct wl_listener *listener, void *data)
{
    struct woodland_server *server = wl_container_of(listener, server, new_output_broadcast);
    struct wlr_output *wlr_output = data;
    if (wlr_output && wlr_output->non_desktop) {
        return;
    }
    /* 协议层可能还没初始化（headless 的首个输出早于 protocol init），
     * 那种情况由 woodland_protocol_init 末尾的补广播覆盖 */
    woodland_protocol_output_changed(server);

    /*
     * 壁纸基线要重新压到图层树之下：output.c 的图层树是 lower_to_bottom()
     * 建的，每来一个输出都会把壁纸顶到 layer-shell background 上面去。
     * 这里必须晚于 handle_new_output（本监听器注册在 protocol init 之后）。
     */
    woodland_protocol_output_added(server, wlr_output);
}

bool woodland_server_init(struct woodland_server *server)
{
    /* ipc_fd 的 0 是 stdin：先置 -1，避免 finish 里误关标准输入 */
    server->ipc_fd = -1;
    wl_list_init(&server->ipc_clients);
    woodland_selfcheck_init(server);

    server->display = wl_display_create();
    if (!server->display) {
        wlr_log(WLR_ERROR, "failed to create wayland display");
        return false;
    }

    wl_display_set_default_max_buffer_size(server->display, 1024 * 1024);

    server->event_loop = wl_display_get_event_loop(server->display);

    server->sigint_source =
        wl_event_loop_add_signal(server->event_loop, SIGINT, handle_terminate, server->display);
    server->sigterm_source =
        wl_event_loop_add_signal(server->event_loop, SIGTERM, handle_terminate, server->display);
    server->sighup_source =
        wl_event_loop_add_signal(server->event_loop, SIGHUP, handle_sighup, server);
    if (!server->sigint_source || !server->sigterm_source || !server->sighup_source) {
        wlr_log(WLR_ERROR, "failed to register signal handler");
        return false;
    }
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);

    struct wlr_session *session = NULL;
    server->backend = wlr_backend_autocreate(server->event_loop, &session);
    if (!server->backend) {
        wlr_log(WLR_ERROR, "failed to create backend");
        return false;
    }

    /*
     * 有 effects 时必须用 scenefx 的 fx renderer：scenefx 的 scene 把
     * output->renderer 直接当 fx_renderer 用，塞一个 wlroots 的 pixman/gles2
     * renderer 进去会按错误的结构布局读内存。
     */
#if WOODLAND_HAVE_EFFECTS
    server->renderer = fx_renderer_create(server->backend);
    if (!server->renderer) {
        wlr_log(WLR_INFO, "no DRM render node, retrying with llvmpipe");
        setenv("WLR_RENDERER_FORCE_SOFTWARE", "1", 1);
        server->renderer = fx_renderer_create(server->backend);
    }
#else
    server->renderer = wlr_renderer_autocreate(server->backend);
#endif
    if (!server->renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        return false;
    }

    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        return false;
    }

    /* 基础协议：compositor 是客户端分配 surface 的必需接口；
     * subcompositor 处理 subsurface；data-device-manager 处理剪贴板。
     * 三者均挂在 display 上，随 display 销毁自动释放。 */
    server->compositor = wlr_compositor_create(server->display, 5, server->renderer);
    wlr_subcompositor_create(server->display);
    wlr_data_device_manager_create(server->display);
    /*
     * wl_shm 不用单独建：上面 wlr_renderer_init_wl_display() 内部已经调过
     * wlr_renderer_init_wl_shm()。再调一次会出现**两个 wl_shm 全局**
     * （wayland-info 能看到 name 1 和 name 5），客户端会按名字各绑一个，
     * 谁都不是"唯一的那份"；冒烟探针里也表现为绑第二个时把第一个代理泄漏掉。
     */

    /* 补充协议：全部是 wlroots 原生 manager，创建即完成接线。
     * alpha_modifier / single_pixel 由 scene 自动集成；
     * fractional_scale 由 scene surface 进入输出时自动 notify。
     * 版本号对齐 labwc（ext_data_control=1、fractional_scale=1）。 */
    wlr_data_control_manager_v1_create(server->display);
    wlr_ext_data_control_manager_v1_create(server->display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->display);
    wlr_fractional_scale_manager_v1_create(server->display, 1);
    wlr_alpha_modifier_v1_create(server->display);
    /* wl_fixes：核心 wayland.xml 协议补丁（destroy_registry），修 wl_registry
     * 的生命周期问题。wlroots 0.20.2 只实现 version 1（ack_global_remove 为
     * since=2），版本号对齐 labwc。 */
    wlr_fixes_create(server->display, 1);

    server->scene = wlr_scene_create();
    if (!server->scene) {
        wlr_log(WLR_ERROR, "failed to create scene");
        return false;
    }
    if (!woodland_output_init(server)) {
        wlr_log(WLR_ERROR, "failed to init output");
        return false;
    }

#if WOODLAND_HAVE_EFFECTS
    /* 模糊参数由 [effects] 配置驱动，按需创建预模糊缓存（需在 output_init 之后） */
    woodland_effect_apply_scene_config(server);
#endif
    if (!woodland_cursor_init(server)) {
        wlr_log(WLR_ERROR, "failed to init cursor");
        return false;
    }

    if (!woodland_seat_init(server)) {
        wlr_log(WLR_ERROR, "failed to init seat");
        return false;
    }

    if (!woodland_ime_init(server)) {
        wlr_log(WLR_ERROR, "failed to init ime");
        return false;
    }
    if (!woodland_session_lock_init(server)) {
        wlr_log(WLR_ERROR, "failed to init session lock");
        return false;
    }
    if (!woodland_shell_init(server)) {
        wlr_log(WLR_ERROR, "failed to init shell");
        return false;
    }

    if (!woodland_workspace_init(server)) {
        wlr_log(WLR_ERROR, "failed to init workspace");
        return false;
    }

    if (!woodland_layer_init(server)) {
        wlr_log(WLR_ERROR, "failed to init layer");
        return false;
    }

    /*
     * 光标监听器必须**早于**协议层注册。
     *
     * wl_signal 的监听器按注册顺序调用，而协议层的 motion 回调要给
     * relative-pointer 客户端发相对位移——它内部取的是
     * seat->pointer_state.focused_client。shell 的回调会更新 pointer focus，
     * 所以它必须排前面；否则跨 surface（或从桌面首次进入客户端）的那一帧，
     * relative motion 会按旧焦点发送：首个 delta 要么被丢弃，要么发给刚
     * 离开的客户端。
     */
    woodland_cursor_init_events(server);

    /* 上游协议：依赖 cursor / seat / output_layout / shell 都已就位，
     * 所以放在最后。内部只做接线，失败即整体失败（缺协议比半个协议好查）。 */
    if (!woodland_protocol_init(server)) {
        wlr_log(WLR_ERROR, "failed to init protocol layer");
        return false;
    }

    /* 新输出的收尾广播。必须在 woodland_output_init 之后注册，才会排在
     * handle_new_output 后面执行——那时输出已经进链表、已 commit，
     * 广播里的 enabled/mode 才是真值。
     *
     * verbose 注册（emit 自动计数）：它是 connect_verbose 的活体示例，
     * 选它是因为启动必发一次——smoke 拿"emits >= 1"断言计账链路是活的；
     * 同时它自己就是坑 2 的契约现场，跑没跑一眼可查 */
    woodland_listener_connect_verbose(&server->new_output_broadcast,
                                      &server->backend->events.new_output,
                                      handle_new_output_broadcast, "server.new_output");

    if (!woodland_capture_init(server)) {
        wlr_log(WLR_ERROR, "failed to init capture");
        return false;
    }

    /* Xwayland：需要 compositor（surface 分配）与 seat（X 输入焦点），
     * 所以放在 seat/shell 之后。lazy 模式：首个 X 客户端连接才拉进程。 */
#if WOODLAND_HAVE_XWAYLAND
    if (!woodland_xwayland_init(server)) {
        wlr_log(WLR_ERROR, "failed to init xwayland");
        return false;
    }
#endif

    server->cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
    if (!server->cursor_shape_manager) {
        wlr_log(WLR_ERROR, "failed to create cursor-shape manager");
        return false;
    }

    woodland_listener_connect(&server->request_set_shape,
                              &server->cursor_shape_manager->events.request_set_shape,
                              handle_request_set_shape, "server.request_set_shape");

    if (!server->config) {
        struct woodland_config *cfg = woodland_config_load(server->config_path);
        woodland_config_apply(server, cfg);
    }
    return true;
}

bool woodland_server_start(struct woodland_server *server)
{
    const char *socket = wl_display_add_socket_auto(server->display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to add wayland socket");
        return false;
    }

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return false;
    }

    setenv("WAYLAND_DISPLAY", socket, true);

    /* 调试接口：路径按 wayland socket 名派生，并导出 WOODLAND_IPC。
     * 起不来只记日志——调试面挂了不该影响合成器本身。 */
    if (!woodland_ipc_init(server, socket)) {
        wlr_log(WLR_ERROR, "debug IPC unavailable, continuing without it");
    }

    /* X 客户端（以及 startup command 拉起的 X 程序）通过 DISPLAY 找 Xwayland */
    if (server->xwayland && server->xwayland->display_name) {
        setenv("DISPLAY", server->xwayland->display_name, true);
        wlr_log(WLR_INFO, "DISPLAY=%s", server->xwayland->display_name);
    }
    if (server->startup_cmd) {
        pid_t pid = fork();
        if (pid < 0) {
            wlr_log(WLR_ERROR, "failed to fork for startup command");
            return false;
        } else if (pid == 0) {
            execl("/bin/sh", "/bin/sh", "-c", server->startup_cmd, (void *)NULL);
        }
    }

    return true;
}

void woodland_server_run(struct woodland_server *server)
{
    wl_display_run(server->display);
    wl_display_destroy_clients(server->display);
}

void woodland_server_finish(struct woodland_server *server)
{
    if (server->sigint_source) {
        wl_event_source_remove(server->sigint_source);
        server->sigint_source = NULL;
    }
    if (server->sigterm_source) {
        wl_event_source_remove(server->sigterm_source);
        server->sigterm_source = NULL;
    }
    if (server->sighup_source) {
        wl_event_source_remove(server->sighup_source);
        server->sighup_source = NULL;
    }
    if (server->config) {
        woodland_config_destroy(server->config);
        server->config = NULL;
    }
    if (server->request_set_shape.link.prev) {
        wl_list_remove(&server->request_set_shape.link);
    }
    if (server->new_output_broadcast.link.prev) {
        wl_list_remove(&server->new_output_broadcast.link);
    }
    woodland_cursor_finish_events(server);
#if WOODLAND_HAVE_XWAYLAND
    woodland_xwayland_finish(server);
#endif
    woodland_selfcheck_finish(server);
    woodland_ipc_finish(server);
    /* 协议层摘掉挂在 cursor/seat/output_layout 上的监听器，
     * 必须早于这些对象被销毁 */
    woodland_protocol_finish(server);
    woodland_capture_finish(server);
    woodland_shell_finish(server);
    woodland_ssd_finish();
    woodland_layer_finish(server);
    woodland_workspace_finish(server);
    woodland_ime_finish(server);
    woodland_session_lock_finish(server);
    woodland_seat_finish(server);
    woodland_cursor_finish(server);
    woodland_output_finish(server);
    if (server->scene) {
        wlr_scene_node_destroy(&server->scene->tree.node);
        server->scene = NULL;
    }
    if (server->allocator) {
        wlr_allocator_destroy(server->allocator);
        server->allocator = NULL;
    }
    if (server->renderer) {
        wlr_renderer_destroy(server->renderer);
        server->renderer = NULL;
    }
    if (server->backend) {
        wlr_backend_destroy(server->backend);
        server->backend = NULL;
    }
    if (server->display) {
        wl_display_destroy(server->display);
        server->display = NULL;
    }
}
