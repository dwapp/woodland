// SPDX-License-Identifier: GPL-2.0-only
/*
 * Alt+Tab 切换器单测：下标轮转、候选快照、高亮移动、中途掉窗口。
 *
 * 不验证"焦点真的换了"——那需要真实 surface 与 seat 键盘 grab，走
 * tests/smoke/switcher_probe.c 的端到端用例（虚拟键盘发真按键）。
 * 这里只钉住"选谁"和"面板状态"，因为这两块出错时最不容易在肉眼上发现：
 * 连按 Tab 弹回同一个窗口、候选被关掉后面板里留着悬空指针。
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <xkbcommon/xkbcommon.h>

#include "focus.h"
#include "multitask_view.h"
#include "output.h"
#include "scene.h"
#include "session_lock.h"
#include "util/json.h"
#include "view.h"
#include "window_switcher.h"
#include "woodland.h"
#include "workspace.h"

/* 把 status 序列化出来做子串断言：struct 对测试是不透明的，这是它唯一的读口 */
static void expect_status(struct woodland_server *server, const char *active,
                          const char *count, const char *index)
{
    struct woodland_json json;
    woodland_json_init(&json);
    woodland_window_switcher_status(server, &json);
    const char *s = woodland_json_data(&json);
    assert(s != NULL);
    assert(strstr(s, active) != NULL);
    assert(strstr(s, count) != NULL);
    assert(strstr(s, index) != NULL);
    woodland_json_finish(&json);
}

int main(void)
{
    /* ---------- 1. 下标轮转（纯函数） ---------- */
    assert(woodland_switcher_next_index(0, 0, true) == -1);
    assert(woodland_switcher_next_index(-3, 0, false) == -1);
    assert(woodland_switcher_next_index(1, 0, true) == 0);
    assert(woodland_switcher_next_index(3, 0, true) == 1);
    assert(woodland_switcher_next_index(3, 2, true) == 0); /* 末尾回绕 */
    assert(woodland_switcher_next_index(3, 0, false) == 2); /* 开头回绕 */
    assert(woodland_switcher_next_index(3, 2, false) == 1);
    /* from 越界（选中项已经被关掉了之类）：按方向取首/尾，不能返回 -1 */
    assert(woodland_switcher_next_index(3, -1, true) == 0);
    assert(woodland_switcher_next_index(3, 7, false) == 2);

    /* ---------- 2. 预选下标 ---------- */
    struct woodland_view va = {0}, vb = {0}, vc = {0};
    struct woodland_view *list[3] = {&va, &vb, &vc};
    /* 焦点在最上那个、往前：越过自己一格，落到 b——这就是"Alt+Tab 换到上一个用过的窗口" */
    assert(woodland_switcher_initial_index(list, 3, &va, true) == 1);
    assert(woodland_switcher_initial_index(list, 3, &va, false) == 2);
    assert(woodland_switcher_initial_index(list, 3, &vc, true) == 0);
    /* 焦点不在候选里（例如在 layer surface 上）：往前取最上、往回取最下 */
    assert(woodland_switcher_initial_index(list, 3, NULL, true) == 0);
    assert(woodland_switcher_initial_index(list, 3, NULL, false) == 2);
    assert(woodland_switcher_initial_index(list, 0, &va, true) == -1);

    /* ---------- 3. 真状态机（要 scene：面板要画出来） ---------- */
    struct woodland_server server = {0};
    server.display = wl_display_create();
    assert(server.display);
    server.event_loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_headless_backend_create(server.event_loop);
    assert(server.backend);
    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator);
    server.scene = wlr_scene_create();
    assert(server.scene);
    assert(woodland_output_init(&server));

    struct wlr_output *out = wlr_headless_add_output(server.backend, 800, 600);
    assert(out != NULL);
    wl_signal_emit_mutable(&server.backend->events.new_output, out);

    server.cursor = wlr_cursor_create();
    assert(server.cursor);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wl_list_init(&server.workspaces);
    wl_list_init(&server.views);
    wl_signal_init(&server.view_mapped);
    wl_signal_init(&server.view_unmapped);
    wl_signal_init(&server.view_changed);

    struct woodland_workspace ws1 = {0};
    ws1.name = "1";
    ws1.server = &server;
    ws1.tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(&server.workspaces, &ws1.link);
    struct woodland_workspace ws2 = {0};
    ws2.name = "2";
    ws2.server = &server;
    ws2.tree = wlr_scene_tree_create(&server.scene->tree);
    wl_list_insert(&server.workspaces, &ws2.link);
    server.active_workspace = &ws1;

    /* z 序（链表头最上）：a b c，焦点在 a */
    va.server = &server;
    va.id = 1;
    va.mapped = true;
    va.workspace = &ws1;
    va.scene_tree = wlr_scene_tree_create(ws1.tree);
    vb.server = &server;
    vb.id = 2;
    vb.mapped = true;
    vb.workspace = &ws1;
    vb.scene_tree = wlr_scene_tree_create(ws1.tree);
    vc.server = &server;
    vc.id = 3;
    vc.mapped = true;
    vc.workspace = &ws1;
    vc.scene_tree = wlr_scene_tree_create(ws1.tree);
    wl_list_insert(&server.views, &vc.link);
    wl_list_insert(&server.views, &vb.link);
    wl_list_insert(&server.views, &va.link);

    /*
     * 假的会话锁：只需要 locked 与 lock_changed 两个字段（is_locked 就是读
     * server->session_lock->locked）。必须在 init 之前挂上去——切换器是在
     * init 里订阅信号的。
     */
    struct woodland_session_lock fake_lock = {0};
    fake_lock.server = &server;
    wl_signal_init(&fake_lock.lock_changed);
    server.session_lock = &fake_lock;

    assert(woodland_window_switcher_init(&server));
    assert(!woodland_window_switcher_is_active(&server));
    expect_status(&server, "\"active\":false", "\"count\":0", "\"index\":-1");

    /* 不在当前工作区的窗口不进候选：ws2 里那个 */
    struct woodland_view vd = {0};
    vd.server = &server;
    vd.id = 4;
    vd.mapped = true;
    vd.workspace = &ws2;
    vd.scene_tree = wlr_scene_tree_create(ws2.tree);
    wl_list_insert(server.views.prev, &vd.link);

    server.focused_view = &va;

    /* ---------- 3b. 拖动/缩放中不接 Alt+Tab ---------- */
    /*
     * 那两个状态键盘归 interactive 那一层（方向键微调、Escape 取消），
     * 开面板会把键盘抢走，并在拖动途中改焦点。
     */
    server.cursor_mode = WOODLAND_CURSOR_MOVE;
    assert(!woodland_window_switcher_begin(&server, true));
    assert(!woodland_window_switcher_is_active(&server));
    server.cursor_mode = WOODLAND_CURSOR_RESIZE;
    assert(!woodland_window_switcher_begin(&server, true));
    server.cursor_mode = WOODLAND_CURSOR_PASSTHROUGH;
    assert(!woodland_window_switcher_is_active(&server));

    /* ---------- 4. 打开：预选 b，面板激活 ---------- */
    assert(woodland_window_switcher_begin(&server, true));
    assert(woodland_window_switcher_is_active(&server));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    /* 高亮项就是 b */
    {
        struct woodland_json json;
        woodland_json_init(&json);
        woodland_window_switcher_status(&server, &json);
        assert(strstr(woodland_json_data(&json), "\"selected\":2") != NULL);
        woodland_json_finish(&json);
    }

    /* ---------- 5. 连按 Tab 只在 3 个候选里循环，不会弹回 ---------- */
    assert(woodland_window_switcher_step(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":2");
    assert(woodland_window_switcher_step(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":0");
    assert(woodland_window_switcher_step(&server, false));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":2");

    /* ---------- 6. 键盘：Tab / Shift+Tab / 方向键 / Escape ---------- */
    /* Tab press 往前走一格（2 → 0）；Shift+Tab（ISO_Left_Tab）往回 */
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_Tab,
                                               WL_KEYBOARD_KEY_STATE_PRESSED, 0));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":0");
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_ISO_Left_Tab,
                                               WL_KEYBOARD_KEY_STATE_PRESSED,
                                               WLR_MODIFIER_SHIFT));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":2");
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_Up,
                                               WL_KEYBOARD_KEY_STATE_PRESSED, 0));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    /* 松开 Tab 不由切换器状态机响应（由 seat 层吞键位图配对吞掉），不改选择也不关面板 */
    assert(!woodland_window_switcher_handle_key(&server, XKB_KEY_Tab,
                                                WL_KEYBOARD_KEY_STATE_RELEASED, 0));
    assert(woodland_window_switcher_is_active(&server));
    /* 其它字符键被吞掉但不动选择 */
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_x,
                                               WL_KEYBOARD_KEY_STATE_PRESSED, 0));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    /* Escape 取消：只关面板 */
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_Escape,
                                               WL_KEYBOARD_KEY_STATE_PRESSED, 0));
    assert(!woodland_window_switcher_is_active(&server));
    expect_status(&server, "\"active\":false", "\"count\":0", "\"index\":-1");

    /* 没激活时按键不被消费（否则 Tab 会被永久吞掉） */
    assert(!woodland_window_switcher_handle_key(&server, XKB_KEY_Tab,
                                                WL_KEYBOARD_KEY_STATE_PRESSED, 0));

    /* ---------- 7. 反向打开：预选最后一个 ---------- */
    assert(woodland_window_switcher_begin(&server, false));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":2");
    /* 松开 Alt = 提交 */
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_Alt_L,
                                               WL_KEYBOARD_KEY_STATE_RELEASED, 0));
    assert(!woodland_window_switcher_is_active(&server));

    /* ---------- 7b. 锁屏：面板立刻收起，不留陈旧高亮 ---------- */
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    assert(woodland_window_switcher_is_active(&server));
    fake_lock.locked = true;
    wl_signal_emit_mutable(&fake_lock.lock_changed, NULL);
    assert(!woodland_window_switcher_is_active(&server));
    /* 解锁不该把面板再弹回来 */
    fake_lock.locked = false;
    wl_signal_emit_mutable(&fake_lock.lock_changed, NULL);
    assert(!woodland_window_switcher_is_active(&server));

    /* ---------- 7c. 切换中候选被最小化：行留着，只是标记变了 ---------- */
    /*
     * 最小化走的是 woodland_view_set_minimized()：关 scene 节点 + 发
     * view_changed，**不发 view_unmapped**。这一行不能摘——Alt+Tab 正是把
     * 最小化窗口叫回来的入口，摘掉等于"最小化的窗口再也切不回来"。
     * 要做的只是把标记改掉（面板上那行加"(已最小化)"、压暗）。
     */
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    /* 前提：最小化之后它自己确实不再是 focusable，但仍然是合法目标 */
    vb.minimized = true;
    assert(!woodland_view_focusable(&vb));
    assert(woodland_view_restorable(&vb));
    wl_signal_emit_mutable(&server.view_changed, &vb);
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    /* 恢复：同一行走回普通外观，行数还是 3 */
    vb.minimized = false;
    wl_signal_emit_mutable(&server.view_changed, &vb);
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");

    /* ---------- 7d. 提交到最小化的目标：先恢复，再聚焦 ---------- */
    /*
     * focus_view 对最小化视图是"恢复 + 继续往下聚焦"（恢复完就 return 的老
     * 写法会让窗口恢复了却拿不到焦点，见 focus.c 那段注释）。这里只钉住
     * "恢复真的发生了"：假视图没有 surface，聚焦本身在这一层观测不到，
     * 端到端由 tests/smoke/switcher_probe.c 用真按键 + get_focus 验。
     */
    vb.minimized = true;
    wl_signal_emit_mutable(&server.view_changed, &vb);
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    assert(woodland_window_switcher_handle_key(&server, XKB_KEY_Alt_L,
                                               WL_KEYBOARD_KEY_STATE_RELEASED, 0));
    assert(!woodland_window_switcher_is_active(&server));
    assert(!vb.minimized);

    /* ---------- 7e. 全部候选都最小化：面板照开 ---------- */
    /*
     * 只剩最小化窗口时最容易写成"不弹空面板"，但那恰好把唯一能恢复它们的
     * 入口关掉了。候选集用的是 restorable 而不是 focusable，就是为了这条。
     */
    struct woodland_view *all3[3] = {&va, &vb, &vc};
    for (int i = 0; i < 3; i++) {
        all3[i]->minimized = true;
    }
    server.focused_view = NULL;
    assert(woodland_window_switcher_begin(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":0");
    for (int i = 0; i < 3; i++) {
        all3[i]->minimized = false;
    }
    woodland_window_switcher_cancel(&server);
    assert(!woodland_window_switcher_is_active(&server));

    /* ---------- 7f. 一次信号里"最小化 + 挪去别的工作区"：摘掉 ---------- */
    /*
     * 这两件事都只走 view_changed，而且真会一起发生。判定顺序要是反过来
     * （先看最小化标记），就会走进"更新标记、重排重画"那条分支然后 return，
     * 面板里留一行**已经切不过去**的候选（直到下一个信号才消失）。
     */
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    vb.minimized = true;
    vb.workspace = &ws2;
    wl_signal_emit_mutable(&server.view_changed, &vb);
    expect_status(&server, "\"active\":true", "\"count\":2", "\"index\":1");
    /* 回到当前工作区也不会自己回来：候选集是打开那一刻拍下来的 */
    vb.minimized = false;
    vb.workspace = &ws1;
    wl_signal_emit_mutable(&server.view_changed, &vb);
    expect_status(&server, "\"active\":true", "\"count\":2", "\"index\":1");
    woodland_window_switcher_cancel(&server);
    assert(!woodland_window_switcher_is_active(&server));

    /* ---------- 7g. 切工作区：面板立刻收起，不留旧工作区快照 ---------- */
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    assert(woodland_window_switcher_is_active(&server));
    server.active_workspace = &ws2;
    wl_signal_emit_mutable(&server.workspace_changed, &ws2);
    assert(!woodland_window_switcher_is_active(&server));
    server.active_workspace = &ws1;

    /* ---------- 7h. 与多任务视图互斥 ---------- */
    assert(woodland_multitask_view_init(&server));
    woodland_multitask_view_open(&server);
    assert(woodland_multitask_view_is_active(&server));
    assert(!woodland_window_switcher_begin(&server, true));
    assert(!woodland_window_switcher_is_active(&server));
    woodland_multitask_view_close(&server, false);
    assert(!woodland_multitask_view_is_active(&server));

    /* 反过来：切换器开着时打开多任务视图，先收起切换器 */
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    assert(woodland_window_switcher_is_active(&server));
    woodland_multitask_view_open(&server);
    assert(!woodland_window_switcher_is_active(&server));
    assert(woodland_multitask_view_is_active(&server));
    woodland_multitask_view_close(&server, false);
    woodland_multitask_view_finish(&server);

    /* ---------- 7i. 拖拽中打开切换器：拖拽图标树仍保持最顶层 ---------- */
    struct wlr_drag fake_drag = {0};
    server.dnd.drag = &fake_drag;
    server.dnd.icons = wlr_scene_tree_create(&server.scene->tree);
    assert(server.dnd.icons != NULL);
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    assert(woodland_window_switcher_is_active(&server));
    {
        struct wl_list *last = server.scene->tree.children.prev;
        struct wlr_scene_node *top = wl_container_of(last, top, link);
        assert(top == &server.dnd.icons->node);
    }
    woodland_window_switcher_cancel(&server);
    server.dnd.drag = NULL;
    wlr_scene_node_destroy(&server.dnd.icons->node);
    server.dnd.icons = NULL;

    /* ---------- 7j. 超长中文 UTF-8 标题 + 最小化后缀：按字符边界截断不崩溃 ---------- */
    {
        char long_zh[320];
        size_t pos = 0;
        const char *unit = "中文测试";
        size_t ulen = strlen(unit);
        while (pos + ulen < sizeof(long_zh) - 1) {
            memcpy(long_zh + pos, unit, ulen);
            pos += ulen;
        }
        long_zh[pos] = '\0';
        va.title = long_zh;
        va.minimized = true;
        server.focused_view = &vb;
        assert(woodland_window_switcher_begin(&server, true));
        assert(woodland_window_switcher_is_active(&server));
        woodland_window_switcher_cancel(&server);
        va.title = NULL;
        va.minimized = false;
    }

    /* ---------- 8. 切换中候选被卸载：从快照里摘掉，不悬空 ---------- */
    server.focused_view = &va;
    assert(woodland_window_switcher_begin(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":3", "\"index\":1");
    /* 被摘的是高亮项（b）→ 高亮落到原位置上的下一个（c，id 3） */
    vb.mapped = false;
    wl_list_remove(&vb.link); /* shell 是先摘链表再回退焦点，切换器只看信号 */
    wl_signal_emit_mutable(&server.view_unmapped, &vb);
    expect_status(&server, "\"active\":true", "\"count\":2", "\"index\":1");
    {
        struct woodland_json json;
        woodland_json_init(&json);
        woodland_window_switcher_status(&server, &json);
        assert(strstr(woodland_json_data(&json), "\"selected\":3") != NULL);
        woodland_json_finish(&json);
    }
    /* 再摘掉一个 → 只剩 a，高亮钳到最后一项 */
    vc.mapped = false;
    wl_list_remove(&vc.link);
    wl_signal_emit_mutable(&server.view_unmapped, &vc);
    expect_status(&server, "\"active\":true", "\"count\":1", "\"index\":0");
    /* 只有一个候选时连按 Tab 也不动 */
    assert(woodland_window_switcher_step(&server, true));
    expect_status(&server, "\"active\":true", "\"count\":1", "\"index\":0");
    /* 最后一个也走掉 → 面板自动收起 */
    va.mapped = false;
    wl_list_remove(&va.link);
    wl_signal_emit_mutable(&server.view_unmapped, &va);
    assert(!woodland_window_switcher_is_active(&server));

    /* ---------- 9. 没有候选（全部 unmap）时不弹空面板 ---------- */
    assert(!woodland_window_switcher_begin(&server, true));
    assert(!woodland_window_switcher_is_active(&server));

    woodland_window_switcher_finish(&server);
    assert(server.window_switcher == NULL);
    server.session_lock = NULL;

    wlr_cursor_destroy(server.cursor);
    woodland_output_finish(&server);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return 0;
}
