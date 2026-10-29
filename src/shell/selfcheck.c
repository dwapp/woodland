// SPDX-License-Identifier: GPL-2.0-only
/*
 * 不变量检查：把"看起来不对"变成"某条断言失败"。
 *
 * 这些检查是在 PR #8 的评审里逼出来的：那轮有五条意见，其中三条
 * （先清 focused_view 再复位旧焦点、unmap 后 foreign-toplevel 仍是激活态、
 * 副屏窗口按主屏工作区最大化）都不是崩溃，而是**状态之间不自洽**，
 * 只能靠人读数像素或读 wlroots 源码去推。写成断言之后，同样的错误
 * 会直接变成一行 "self-check: x failed (expected .. actual ..)"。
 *
 * 检查全部是只读的：不改状态、不下发 configure、不发焦点事件。
 */
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "selfcheck.h"
#include "util/json.h"
#include "ssd.h"
#include "view.h"
#include "woodland.h"
#include "workspace.h"

/* 链表遍历的硬上限：检测到环时不会把合成器卡死在这里 */
#define SELFCHECK_LIST_LIMIT 4096

/* ==================== 小工具 ==================== */

static void set_text(char *dst, size_t len, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(dst, len, fmt, args);
    va_end(args);
}

/* 视图在 views 链表里？（链表很短，线性找足够，不必维护反查表） */
static bool view_in_list(struct woodland_server *server,
                         const struct woodland_view *view) {
    struct woodland_view *it;
    wl_list_for_each(it, &server->views, link) {
        if (it == view) {
            return true;
        }
    }
    return false;
}

static const char *view_id_str(char *buf, size_t len,
                               const struct woodland_view *view) {
    if (!view) {
        snprintf(buf, len, "none");
    } else {
        snprintf(buf, len, "view#%u", view->id);
    }
    return buf;
}

static int ft_state(const struct woodland_view *view) {
    return view->toplevel_handle ? (int)view->toplevel_handle->state : 0;
}

/* ==================== 各条检查 ==================== */

/*
 * views 链表完整性：手写遍历（wl_list_for_each 遇到环会无限转），
 * 同时要求链表里的视图都带 scene_tree，且 mapped 状态与"在链表里"一致。
 *
 * 这条能抓到的是"摘链表只摘了一半"：unmap 里删了 link 却没清 mapped，
 * 或者反过来，后续所有遍历都会把已卸载的窗口当成活窗口。
 */
static void check_views_list(struct woodland_server *server,
                             struct woodland_check *check) {
    bool ok = true;
    int count = 0;
    int mismatched_mapped = 0;
    char actual[WOODLAND_SELFCHECK_TEXT];

    if (!server->views.next) {
        /* 链表没初始化：调用方漏了 wl_list_init(&server->views) */
        check->ok = false;
        set_text(check->expected, sizeof(check->expected),
                 "views list initialised");
        set_text(check->actual, sizeof(check->actual), "views.next is NULL");
        return;
    }

    struct wl_list *pos = server->views.next;
    while (pos && pos != &server->views && count <= SELFCHECK_LIST_LIMIT) {
        struct woodland_view *view = wl_container_of(pos, view, link);
        count++;
        if (!view->mapped) {
            /* 在链表里但 mapped 为假：unmap 的中途状态或漏置标志 */
            mismatched_mapped++;
            ok = false;
        }
        if (!view->scene_tree) {
            ok = false;
        }
        pos = pos->next;
    }
    if (pos != &server->views) {
        /* 超过上限还没回到表头：链表有环或被写坏 */
        set_text(check->actual, sizeof(check->actual),
                 "list did not terminate within %d nodes", SELFCHECK_LIST_LIMIT);
        check->ok = false;
        set_text(check->expected, sizeof(check->expected),
                 "list terminates at sentinel");
        return;
    }

    set_text(actual, sizeof(actual), "%d views, %d mapped=0 in list",
             count, mismatched_mapped);
    check->ok = ok;
    set_text(check->expected, sizeof(check->expected),
             "all listed views mapped and with scene_tree");
    set_text(check->actual, sizeof(check->actual), "%s", actual);
}

/*
 * 已映射视图的工作区归属：workspace 非空，且 scene_tree 真的挂在该
 * 工作区的场景树下。
 *
 * 这条防的是"窗口还在旧工作区的树里"——切换工作区时 reparent 漏了，
 * 表现是窗口在当前桌面看不见、在别的桌面看得见，非常难查。
 */
static void check_workspace_membership(struct woodland_server *server,
                                       struct woodland_check *check) {
    int bad = 0;
    unsigned bad_id = 0;
    char what[64] = "ok";

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->mapped) {
            continue;
        }
        if (!view->workspace) {
            bad++;
            bad_id = view->id;
            snprintf(what, sizeof(what), "no workspace");
            continue;
        }
        if (!view->scene_tree) {
            bad++;
            bad_id = view->id;
            snprintf(what, sizeof(what), "no scene_tree");
            continue;
        }
        if (view->scene_tree->node.parent != view->workspace->tree) {
            bad++;
            bad_id = view->id;
            snprintf(what, sizeof(what), "scene_tree not under workspace tree");
        }
    }

    check->ok = (bad == 0);
    set_text(check->expected, sizeof(check->expected),
             "0 mapped views misplaced");
    if (bad == 0) {
        set_text(check->actual, sizeof(check->actual), "0 misplaced");
    } else {
        set_text(check->actual, sizeof(check->actual),
                 "%d misplaced, first view#%u: %s", bad, bad_id, what);
    }
}

/*
 * focused_view 的有效性：在链表里、可聚焦、有 surface 可以收键盘。
 *
 * 另外核对 seat 的键盘 surface：有键盘设备时键盘焦点必须指向同一个视图，
 * 否则就是"标题栏亮着但按键跑到别处"。
 */
static void check_focused_view(struct woodland_server *server,
                               struct woodland_check *check) {
    struct woodland_view *focused = server->focused_view;
    if (!focused) {
        check->ok = true;
        set_text(check->expected, sizeof(check->expected), "no focused view");
        set_text(check->actual, sizeof(check->actual), "no focused view");
        return;
    }

    char id[32];
    view_id_str(id, sizeof(id), focused);

    if (!view_in_list(server, focused)) {
        check->ok = false;
        set_text(check->expected, sizeof(check->expected),
                 "%s present in views list", id);
        set_text(check->actual, sizeof(check->actual),
                 "%s missing from list", id);
        return;
    }
    if (!woodland_view_focusable(focused)) {
        check->ok = false;
        set_text(check->expected, sizeof(check->expected),
                 "%s focusable", id);
        set_text(check->actual, sizeof(check->actual),
                 "%s not focusable (mapped=%d minimized=%d orid=%d same_ws=%d)",
                 id, focused->mapped, focused->minimized,
                 focused->override_redirect,
                 focused->workspace == server->active_workspace);
        return;
    }
    if (!woodland_view_surface(focused)) {
        check->ok = false;
        set_text(check->expected, sizeof(check->expected),
                 "%s has a wlr_surface", id);
        set_text(check->actual, sizeof(check->actual),
                 "%s has no surface", id);
        return;
    }

    /* 键盘实际指向谁：只有 seat 里确实记着一个属于视图的 surface 时才比 */
    if (server->seat) {
        struct wlr_surface *kbd =
            server->seat->keyboard_state.focused_surface;
        if (kbd) {
            struct woodland_view *from_kbd = woodland_view_from_surface(kbd);
            if (from_kbd && from_kbd != focused) {
                char kbd_id[32];
                view_id_str(kbd_id, sizeof(kbd_id), from_kbd);
                check->ok = false;
                set_text(check->expected, sizeof(check->expected),
                         "keyboard surface belongs to %s", id);
                set_text(check->actual, sizeof(check->actual),
                         "keyboard surface belongs to %s", kbd_id);
                return;
            }
        }
    }

    check->ok = true;
    set_text(check->expected, sizeof(check->expected), "%s focusable", id);
    set_text(check->actual, sizeof(check->actual), "%s ok", id);
}

/*
 * 激活态唯一：至多一个视图 activated，且必须是 focused_view。
 *
 * 这正是 PR #8 评审第 4 条的形状：unmap 里只清 focused_view 不复位旧视图，
 * 有键盘时靠反查还能补救，无键盘（headless）时没人负责变暗，
 * 结果两扇窗口的标题栏同时是激活色。断言写成"恰好一个且等于焦点"。
 */
static void check_activated_unique(struct woodland_server *server,
                                   struct woodland_check *check) {
    int activated = 0;
    unsigned activated_id = 0;
    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->activated) {
            activated++;
            activated_id = view->id;
        }
    }

    if (activated == 0) {
        check->ok = (server->focused_view == NULL);
        set_text(check->expected, sizeof(check->expected),
                 "0 activated views matches no focus");
        if (check->ok) {
            set_text(check->actual, sizeof(check->actual), "0 activated, no focus");
        } else {
            char id[32];
            view_id_str(id, sizeof(id), server->focused_view);
            set_text(check->actual, sizeof(check->actual),
                     "0 activated but %s is focused", id);
        }
        return;
    }

    if (activated > 1) {
        check->ok = false;
        set_text(check->expected, sizeof(check->expected),
                 "at most 1 activated view");
        set_text(check->actual, sizeof(check->actual),
                 "%d activated views", activated);
        return;
    }

    check->ok = (server->focused_view && server->focused_view->id == activated_id
                 && server->focused_view->activated);
    set_text(check->expected, sizeof(check->expected),
             "the activated view is the focused view");
    if (check->ok) {
        set_text(check->actual, sizeof(check->actual), "view#%u both", activated_id);
    } else {
        char id[32];
        view_id_str(id, sizeof(id), server->focused_view);
        set_text(check->actual, sizeof(check->actual),
                 "view#%u activated but focus is %s", activated_id, id);
    }
}

/* SSD 标题栏颜色与激活态一致（三态同步里最容易漏的一处） */
static void check_ssd_active(struct woodland_server *server,
                             struct woodland_check *check) {
    int bad = 0;
    unsigned bad_id = 0;
    bool bad_ssd = false;
    bool bad_view = false;

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->ssd) {
            continue;
        }
        bool ssd_active = woodland_ssd_is_active(view);
        if (ssd_active != view->activated) {
            bad++;
            bad_id = view->id;
            bad_ssd = ssd_active;
            bad_view = view->activated;
        }
    }

    check->ok = (bad == 0);
    set_text(check->expected, sizeof(check->expected),
             "SSD active colour equals view activated state");
    if (bad == 0) {
        set_text(check->actual, sizeof(check->actual), "0 mismatched SSDs");
    } else {
        set_text(check->actual, sizeof(check->actual),
                 "%d mismatched, first view#%u: ssd=%d activated=%d",
                 bad, bad_id, (int)bad_ssd, (int)bad_view);
    }
}

/* foreign-toplevel 的激活位（任务栏/切换器看到的那一份） */
static void check_ft_activated(struct woodland_server *server,
                               struct woodland_check *check) {
    int bad = 0;
    unsigned bad_id = 0;

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->toplevel_handle) {
            continue;
        }
        bool ft = (ft_state(view)
                   & WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) != 0;
        if (ft != view->activated) {
            bad++;
            bad_id = view->id;
        }
    }

    check->ok = (bad == 0);
    set_text(check->expected, sizeof(check->expected),
             "foreign-toplevel ACTIVE bit equals activated state");
    if (bad == 0) {
        set_text(check->actual, sizeof(check->actual), "0 mismatched handles");
    } else {
        set_text(check->actual, sizeof(check->actual),
                 "%d mismatched, first view#%u", bad, bad_id);
    }
}

/*
 * xdg configure 的 activated 位。
 *
 * 用 scheduled 而不是 current：set_activated 只写 scheduled，
 * current 要等客户端 ack，检查它必然在 ack 之前假警报。
 */
static void check_xdg_activated(struct woodland_server *server,
                                struct woodland_check *check) {
    int bad = 0;
    unsigned bad_id = 0;

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->type != WOODLAND_VIEW_XDG || !view->xdg_toplevel) {
            continue;
        }
        if (view->xdg_toplevel->scheduled.activated != view->activated) {
            bad++;
            bad_id = view->id;
        }
    }

    check->ok = (bad == 0);
    set_text(check->expected, sizeof(check->expected),
             "xdg scheduled.activated equals activated state");
    if (bad == 0) {
        set_text(check->actual, sizeof(check->actual), "0 mismatched toplevels");
    } else {
        set_text(check->actual, sizeof(check->actual),
                 "%d mismatched, first view#%u", bad, bad_id);
    }
}

/*
 * SSD 缓存的几何 == 当前内容尺寸。
 *
 * ssd_update() 只在 commit / 状态变化时跑；少调一次的表现是标题栏宽度
 * 停在旧尺寸（窗口拉宽后按钮浮在中间），肉眼看得出来但不好定位。
 */
static void check_ssd_geometry(struct woodland_server *server,
                               struct woodland_check *check) {
    int bad = 0;
    unsigned bad_id = 0;
    int ssd_w = 0, ssd_h = 0, content_w = 0, content_h = 0;

    struct woodland_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->ssd || !woodland_view_surface(view)) {
            continue;
        }
        int w = 0, h = 0;
        if (!woodland_ssd_content_size(view, &w, &h)) {
            continue;
        }
        struct wlr_box content;
        woodland_view_content_box(view, &content);
        if (w != content.width || h != content.height) {
            bad++;
            bad_id = view->id;
            ssd_w = w;
            ssd_h = h;
            content_w = content.width;
            content_h = content.height;
        }
    }

    check->ok = (bad == 0);
    set_text(check->expected, sizeof(check->expected),
             "SSD cached size equals content size");
    if (bad == 0) {
        set_text(check->actual, sizeof(check->actual), "0 mismatched SSDs");
    } else {
        set_text(check->actual, sizeof(check->actual),
                 "%d mismatched, first view#%u: ssd=%dx%d content=%dx%d",
                 bad, bad_id, ssd_w, ssd_h, content_w, content_h);
    }
}

/* ==================== 汇总 ==================== */

int woodland_selfcheck_collect(struct woodland_server *server,
                               struct woodland_check *out, int max) {
    if (!server || !out || max <= 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out) * (size_t)max);

    /* 顺序有意义：先看链表本身可用，后面的遍历才有意义 */
    static const struct {
        const char *name;
        void (*run)(struct woodland_server *, struct woodland_check *);
    } checks[] = {
        {"views_list_intact", check_views_list},
        {"mapped_in_workspace", check_workspace_membership},
        {"focused_view_valid", check_focused_view},
        {"activated_unique", check_activated_unique},
        {"ssd_active_matches", check_ssd_active},
        {"ft_activated_matches", check_ft_activated},
        {"xdg_activated_matches", check_xdg_activated},
        {"ssd_geometry_matches", check_ssd_geometry},
    };

    int count = 0;
    int n = (int)(sizeof(checks) / sizeof(checks[0]));
    for (int i = 0; i < n && count < max; i++) {
        struct woodland_check *check = &out[count++];
        check->name = checks[i].name;
        check->ok = true;
        checks[i].run(server, check);
    }
    return count;
}

/* JSON 字符串里 name/expected/actual 都可能是空串，用 "" 而不是 null */
static void append_text(struct woodland_json *out, const char *key,
                        const char *value) {
    woodland_json_key(out, key);
    woodland_json_string(out, value && value[0] ? value : "-");
}

int woodland_selfcheck_run(struct woodland_server *server,
                           struct woodland_json *out) {
    struct woodland_check checks[WOODLAND_SELFCHECK_MAX];
    int n = woodland_selfcheck_collect(server, checks, WOODLAND_SELFCHECK_MAX);
    int failures = 0;
    for (int i = 0; i < n; i++) {
        if (!checks[i].ok) {
            failures++;
        }
    }

    woodland_json_key(out, "ok");
    woodland_json_bool(out, failures == 0);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "failures");
    woodland_json_int(out, failures);
    woodland_json_raw(out, ",");
    woodland_json_key(out, "checks");
    woodland_json_raw(out, "[");
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            woodland_json_raw(out, ",");
        }
        woodland_json_raw(out, "{");
        woodland_json_key(out, "name");
        woodland_json_string(out, checks[i].name);
        woodland_json_raw(out, ",");
        woodland_json_key(out, "ok");
        woodland_json_bool(out, checks[i].ok);
        if (!checks[i].ok) {
            woodland_json_raw(out, ",");
            append_text(out, "expected", checks[i].expected);
            woodland_json_raw(out, ",");
            append_text(out, "actual", checks[i].actual);
        }
        woodland_json_raw(out, "}");
    }
    woodland_json_raw(out, "]");
    return failures;
}

/* ==================== 调度 ==================== */

static void selfcheck_idle(void *data) {
    struct woodland_server *server = data;
    /* idle 源在回调返回后自动销毁，指针必须在这里清掉，
     * 否则下次 schedule 会误以为还有一个待跑的检查 */
    server->selfcheck_idle = NULL;

    struct woodland_check checks[WOODLAND_SELFCHECK_MAX];
    int n = woodland_selfcheck_collect(server, checks, WOODLAND_SELFCHECK_MAX);
    for (int i = 0; i < n; i++) {
        if (checks[i].ok) {
            continue;
        }
        wlr_log(WLR_ERROR, "self-check %s failed after %s: expected %s, got %s",
                checks[i].name,
                server->selfcheck_reason ? server->selfcheck_reason : "?",
                checks[i].expected, checks[i].actual);
    }
}

void woodland_selfcheck_schedule(struct woodland_server *server,
                                 const char *reason) {
    if (!server || !server->self_check || server->selfcheck_idle) {
        return;
    }
    if (!server->event_loop) {
        return;
    }
    server->selfcheck_reason = reason;
    server->selfcheck_idle =
        wl_event_loop_add_idle(server->event_loop, selfcheck_idle, server);
}

void woodland_selfcheck_init(struct woodland_server *server) {
    if (!server) {
        return;
    }
    server->selfcheck_idle = NULL;
    server->selfcheck_reason = NULL;
}

void woodland_selfcheck_finish(struct woodland_server *server) {
    if (!server) {
        return;
    }
    if (server->selfcheck_idle) {
        wl_event_source_remove(server->selfcheck_idle);
        server->selfcheck_idle = NULL;
    }
}
