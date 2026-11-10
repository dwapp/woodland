// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_PROTOCOL_H
#define WOODLAND_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct woodland_server;
struct woodland_workspace;
struct woodland_view;
struct woodland_json;
struct wlr_input_device;
struct wl_client;
struct wlr_security_context_v1_state;
struct wlr_output;

/*
 * 上游协议接线（src/protocol）。
 *
 * 这一层的职责只有"把 wlroots 的 manager 挂上 display，并把事件接到
 * 已有的窗口管理逻辑上"——不实现协议本身，也不碰渲染与几何。
 *
 * 内部按关注点分了七个文件，各自的取舍写在文件头：
 *   idle.c    空闲与抑制：idle-notify / idle-inhibit / shortcuts-inhibit
 *   output.c  输出管理：output-management / output-power-management
 *   input.c   输入与选区：relative-pointer / pointer-gestures / primary-selection
 *   window.c  窗口：xdg-activation / xdg-toplevel-tag
 *   decoration.c 窗口装饰：treeland-decoration（按窗口的 SSD 定制）
 *   background_effect.c 背景模糊：ext-background-effect-v1
 *   wallpaper.c 壁纸：treeland-wallpaper-shell / -manager / -notifier
 *   compositor_action.c shell 动作：treeland-compositor-action（工作区切换）
 *   appearance.c 用户级外观：treeland-appearance（观测）/ -manager（特权配置）
 *   shortcut.c 全局快捷键与一次性捕获：treeland-shortcut-manager-v3
 *   foreign_toplevel.c 窗口列表：treeland-foreign-toplevel-manager-v3
 *   security_context.c 沙箱隔离与特权过滤：wp-security-context-v1（对齐 labwc）
 *   protocol.c 只管按序建立、逆序回收，以及下面这几个转发入口。
 *
 * 按语义完备程度分档，档位决定了它是否真的改变行为：
 *
 *   语义完整：viewporter、presentation-time、primary-selection、
 *             idle-notify、idle-inhibit、keyboard-shortcuts-inhibit、
 *             output-power-management、output-management、gamma-control、
 *             drm-lease、relative-pointer、pointer-constraints、pointer-gestures、
 *             linux-drm-syncobj、tearing-control、xdg-activation、
 *             treeland-decoration 的边框/标题栏/圆角、
 *             treeland-appearance（bind 即推送 + 变化广播，9 项真生效）、
 *             treeland-appearance-manager 的 9 条 set_*（同上一一对应）、
 *             ext-background-effect（capability 随 [effects] 运行期变化）、
 *             wp-security-context-v1（按 Wayland 连接隔离：白名单过滤特权协议、
 *             禁止嵌套沙箱；默认放行 zwp_keyboard_shortcuts_inhibit_manager_v1；
 *             仅对沙箱引擎通过 create_listener + commit 建立的私有 socket 连接生效，
 *             X11/Xwayland 应用与沙盒内直连主 WAYLAND_DISPLAY 的进程不受限；
 *             新增任何暴露给客户端的 global 时必须同步更新 security_context.c 的
 *             allowed_protocols / privileged_protocols 分类）
 *             —— 客户端请求会真的影响合成器行为。
 *   委派实现：treeland-wallpaper-shell / -manager / -notifier
 *             —— 配置、场景托管、播放控制都在合成器里，但**解码不在**：
 *             图片/视频由外部 treeland-wallpaper-factory 渲染（见设计规格）。
 *   部分实现：treeland-compositor-action 的 workspace_1..12 / prev / next
 *             —— 真接 woodland_workspace_switch()；其余 15..30 号动作
 *             （show-desktop、多任务、缩放、FPS、锁屏、关机菜单、用户切换）
 *             置空但可观测（trace / wctl / 日志），不假装生效。bind 的
 *             "非特权客户端"判定只做到同 uid，woodland 没有 DDE 的
 *             privileged 服务，取舍写在 compositor_action.c 文件头。
 *   部分实现：treeland-foreign-toplevel-manager-v3 的 dock preview context——
 *             对象与请求照协议校验（invalid_surface / context_already_exists），
 *             但缩略图/tooltip popover 是 treeland 的 QML 特效，woodland 不实现，
 *             因此 show / show_tooltip_surface 只记账（trace + wctl），enter /
 *             leave 永不发送。窗口列表、状态、activate/close/maximize/minimize/
 *             fullscreen 这些**语义**部分是完整的。
 *   treeland-shortcut-manager-v3：acquire 独占、bind/unbind 的四种
 *             bind_failure、capture 的 busy/not_active/aborted/interrupted
 *             与 activated 事件语义完整；动作 0..20、22..23、32..39 真接
 *             窗口/工作区/贴边逻辑，21 与 24..31、40..45 置空但可观测。
 *             swipe/hold 绑定能注册、查重、unbind，但 woodland 没有触摸
 *             手势识别状态机，只登记不触发（trace 明写 gesture_unwired）。
 *             多用户 session 的延迟 bind 与真正的特权判定同样未做，
 *             取舍写在 shortcut.c 文件头。
 *   仅记录：  xdg-toplevel-tag（存下来、经 wctl 可见，但暂不参与窗口规则）；
 *             treeland-decoration 的阴影（要一层模糊 buffer，还没做）；
 *             treeland-appearance-manager 的 monospace_font（没有等宽字体
 *             消费方）与 window_opacity（要逐窗口 alpha，属单独一版）；
 *             treeland-foreign-toplevel-manager-v3 的 set_icon_geometry
 *             （最小化动画的提示，woodland 没有动画）。
 *   暂不接线：content-type（scene 不消费）、session-lock / xdg-foreign
 *             （各自需要一套独立状态机，见里程碑文档）。
 */
bool woodland_protocol_init(struct woodland_server *server);
void woodland_protocol_finish(struct woodland_server *server);

/**
 * 把新出现的输入设备交给协议层：手势协议要按设备挂监听。
 *
 * 由 seat 在设备接入时调用（协议层不该自己去抢 backend 的 new_input）。
 */
void woodland_protocol_attach_input(struct woodland_server *server,
                                    struct wlr_input_device *device);

/** 有输入事件时调用，用来给 idle-notify 续命。 */
void woodland_protocol_notify_activity(struct woodland_server *server);

/** 当前焦点 surface 是否持有生效中的快捷键抑制器（有则跳过合成器键位）。 */
bool woodland_protocol_shortcuts_inhibited(struct woodland_server *server);

/**
 * 输出集合或状态变了，让协议层补一次 output-management 配置广播。
 *
 * 布局变化会自动触发广播（协议层监听 output_layout），但有些变化不经过
 * 布局：DPMS 开关、以及 headless 输出 commit 时没有 preferred mode 可供
 * 重算布局。这些路径需要显式通知一声。
 */
void woodland_protocol_output_changed(struct woodland_server *server);


/**
 * 配置热重载之后，把新的模糊能力位广播给已连接的客户端。
 *
 * ext-background-effect 的 capability 在 bind 时发一次，客户端之后不会再问；
 * 所以 [effects] 开关变了要主动给所有已绑定的 manager 补一条（协议允许重复
 * 发送，以最后一条为准）。
 */
void woodland_protocol_refresh_background_effect(struct woodland_server *server);

/** 将 DRM 输出提议给 DRM lease 管理器 */
void woodland_protocol_offer_drm_lease(struct woodland_server *server,
                                       struct wlr_output *output);

/** 从 DRM lease 管理器撤回 DRM 输出提议 */
void woodland_protocol_withdraw_drm_lease(struct woodland_server *server,
                                          struct wlr_output *output);

/**
 * 新输出的 scene 图元（scene_output + 4 棵 layer tree）建好之后调用。
 *
 * 必须在新输出的 handle_new_output 之后跑：图层树用 lower_to_bottom() 建立，
 * 会把壁纸基线顶到 layer-shell 的 background 之上。这是"壁纸永远在
 * layer-shell 下面"的唯一保证，热插拔路径（不只是启动）也要覆盖。
 */
void woodland_protocol_output_added(struct woodland_server *server,
                                    struct wlr_output *output);

/* ==================== treeland-compositor-action ==================== */

/*
 * 前三个是纯函数（不碰 display、不连客户端）：动作表、工作区名、相邻
 * 工作区都能在栈上验证，不必为了测它们起一个合成器；最后一个是只读
 * getter。协议实现与单元测试共用同一份推导，免得测试自己抄一遍 1..12
 * 的映射。
 */
enum woodland_protocol_action_class {
    WOODLAND_PROTO_ACTION_INVALID,       /* 枚举之外的值（0、>30） */
    WOODLAND_PROTO_ACTION_WORKSPACE,    /* workspace_1..12 */
    WOODLAND_PROTO_ACTION_WORKSPACE_PREV,
    WOODLAND_PROTO_ACTION_WORKSPACE_NEXT,
    WOODLAND_PROTO_ACTION_UNSUPPORTED,  /* 15..30：woodland 暂无对应状态机 */
};

enum woodland_protocol_action_class
woodland_protocol_action_classify(uint32_t action);

/** workspace_N -> 名字 "1".."12"；其它动作或缓冲不足返回 false。 */
bool woodland_protocol_action_workspace_name(uint32_t action, char *buf, size_t len);

/**
 * 当前工作区在 server->workspaces 上的前/后一个；边界不绕回（没有上一个
 * 就属于"不能执行"，协议要求忽略）。server 可以是栈上只填了 workspaces
 * 链表的假结构。
 */
struct woodland_workspace *woodland_protocol_action_adjacent_workspace(
    struct woodland_server *server, bool next);

/**
 * 最近一次 trigger 的处置结果，给 wctl get_tree 用。出参都可为 NULL；
 * 协议层没建起来时 available=false、计数为 0、last_outcome=NULL。
 */
void woodland_protocol_compositor_action_status(struct woodland_server *server,
                                                bool *available,
                                                uint32_t *trigger_count,
                                                uint32_t *last_action,
                                                const char **last_outcome);

/* ==================== treeland-shortcut-manager-v3 ==================== */

/*
 * 下面这组是不依赖 display / wlroots 的纯函数：QKeySequence PortableText
 * 的解析、匹配与动作分类都能在栈上验证，单元测试不必起合成器。协议实现
 * 与测试共用同一份推导，避免各自抄一遍 0..45 的动作表。
 */
enum woodland_shortcut_mod {
    WOODLAND_SC_MOD_CTRL  = 1u << 0,
    WOODLAND_SC_MOD_ALT   = 1u << 1,
    WOODLAND_SC_MOD_SHIFT = 1u << 2,
    WOODLAND_SC_MOD_META  = 1u << 3,
};

struct woodland_shortcut_key {
    uint32_t mods;     /* enum woodland_shortcut_mod 的位或 */
    uint32_t sym;      /* xkb_keysym_t（uint32_t）；主键，不含修饰键 */
    char text[64];     /* 规范化后的 PortableText，如 "Ctrl+Alt+Delete" */
};

enum woodland_shortcut_parse_result {
    WOODLAND_SC_PARSE_OK = 0,
    WOODLAND_SC_PARSE_EMPTY,
    WOODLAND_SC_PARSE_SEQUENCE,      /* 多段序列 "A, B"，协议只收单个组合 */
    WOODLAND_SC_PARSE_DUPLICATE_MOD,
    WOODLAND_SC_PARSE_BAD_TOKEN,
    WOODLAND_SC_PARSE_NO_KEY,
    WOODLAND_SC_PARSE_TOO_LONG,
};

enum woodland_shortcut_parse_result
woodland_shortcut_key_parse(const char *text, struct woodland_shortcut_key *out);

/** 稳定字符串（冒烟脚本按名字断言）；未知结果返回 "unknown"。 */
const char *woodland_shortcut_parse_name(enum woodland_shortcut_parse_result result);

/** 把 (mods, sym) 格式化成 PortableText；缓冲不足返回 false。 */
bool woodland_shortcut_key_format(uint32_t mods, uint32_t sym, char *buf, size_t len);

/** capture_next_shortcut 协议里定义的「有效快捷键」判定。 */
bool woodland_shortcut_key_is_valid(uint32_t mods, uint32_t sym);

/** 绑定与按键事件是否同一个组合（sym 按大小写不敏感比较）。 */
bool woodland_shortcut_key_matches(const struct woodland_shortcut_key *key,
                                    uint32_t mods, uint32_t sym);

enum woodland_shortcut_action_class {
    WOODLAND_SC_ACTION_INVALID,
    WOODLAND_SC_ACTION_NOTIFY,
    WOODLAND_SC_ACTION_WORKSPACE,
    WOODLAND_SC_ACTION_WORKSPACE_PREV,
    WOODLAND_SC_ACTION_WORKSPACE_NEXT,
    WOODLAND_SC_ACTION_VIEW,          /* 最大化/最小化/关闭/移动/缩放 */
    WOODLAND_SC_ACTION_MOVE_WORKSPACE,
    WOODLAND_SC_ACTION_TILE,
    WOODLAND_SC_ACTION_UNSUPPORTED,    /* woodland 暂无对应 shell 状态机 */
};

enum woodland_shortcut_action_class
woodland_shortcut_action_classify(uint32_t action);

struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct woodland_json;

/**
 * 键盘事件先过 shortcut 族：返回 true 表示已被 capture 或某个绑定消费，
 * seat 不再把按键发给焦点 surface，也不再走内置 Alt+ 键位。
 *
 * 由 seat 的 keyboard_handle_key 在处理内置键位**之前**调用；capture 的
 * 优先级高于注册绑定，符合协议「capture intercepts input with priority
 * over registered shortcut bindings」。
 */
bool woodland_protocol_shortcuts_key(struct woodland_server *server,
                                     struct wlr_keyboard *keyboard,
                                     const struct wlr_keyboard_key_event *event);

/**
 * 指针 button / wheel 事件入口：有 pending capture 时按协议发
 * failed(interrupted) 并吞掉事件；否则返回 false，光标逻辑照常走。
 */
bool woodland_protocol_shortcuts_pointer(struct woodland_server *server);

/** 把 shortcut 族的只读状态写成一个完整 JSON 对象，给 wctl get_tree 用。 */
void woodland_protocol_shortcut_status(struct woodland_server *server,
                                       struct woodland_json *out);

/* ==================== treeland-appearance / -manager ==================== */

/*
 * set_* 请求的处置分档。
 *
 * 表和实现放在一起（src/protocol/appearance.c），这里是它的公开视图：wctl 的
 * recorded_only 列表和单元测试都从这张表生成，避免"改了一个请求的档位、
 * 忘了同步文档/测试"。
 */
enum woodland_appearance_effect {
    WOODLAND_APPEARANCE_APPLIED = 0, /* 有渲染消费方，客户端能看到变化 */
    WOODLAND_APPEARANCE_RECORDED,    /* 只记录：woodland 还没有消费方 */
};

struct woodland_appearance_setting {
    const char *request;  /* 协议请求名，如 "set_cursor_theme" */
    enum woodland_appearance_effect effect;
    const char *consumer; /* 谁消费它；RECORDED 时恒为 NULL */
};

/** 全部 set_* 的分档表（11 项）。count 出参可为 NULL。 */
const struct woodland_appearance_setting *woodland_appearance_settings(size_t *count);

/** 按请求名查分档；未知返回 NULL。 */
const struct woodland_appearance_setting *woodland_appearance_setting_find(const char *request);

/*
 * 越界裁剪：协议要求把 0 / 超大值收到"合成器支持的范围"，而不是回一个错误
 * （两张 XML 都没有错误枚举）。纯函数，单测直接调，不必起合成器。
 */
uint32_t woodland_appearance_clamp_cursor_size(uint32_t value);
uint32_t woodland_appearance_clamp_titlebar_height(uint32_t value); /* 0 合法 = 隐藏 */
uint32_t woodland_appearance_clamp_corner_radius(uint32_t value);
uint32_t woodland_appearance_clamp_font_size(uint32_t value); /* points * 10 */

/** 外观状态（观测者/管理者数量、当前值、最近一次处置）写进 get_tree。 */
void woodland_protocol_appearance_status(struct woodland_server *server,
                                         struct woodland_json *out);

/**
 * 配置热重载之后把当前外观全量重播给观测端。
 *
 * 重载会把协议写进 config 的运行期值重置回文件值，订阅 treeland-appearance
 * 的客户端必须收到变化；否则控制中心显示的还是它刚设的深色，而合成器已经
 * 回到文件里的浅色。
 */
void woodland_protocol_refresh_appearance(struct woodland_server *server);

/* ==================== treeland-foreign-toplevel-manager-v3 ==================== */

/*
 * 一个窗口的状态位。协议用 uint32 数组表达，位在上面的顺序就是 XML 里
 * state 枚举的取值（maximized=0 … attention=4）。
 *
 * 抽成纯函数是因为它是唯一"算错了客户端就理解错"的地方：状态事件的数组
 * 少一位、顺序反了，都不会崩，只会让 Dock 的图标一直显示成最大化的样子。
 */
struct woodland_toplevel_state {
    bool maximized;
    bool minimized;
    bool activated;
    bool fullscreen;
    bool attention;
};

/**
 * 编码成协议要的 uint32 数组（升序、只含置位的项），返回写入个数。
 *
 * cap 不足时截断并返回 cap：调用方的缓冲按 5 个开，正常永远够。
 */
size_t woodland_protocol_toplevel_state_encode(const struct woodland_toplevel_state *state,
                                               uint32_t *out, size_t cap);

/** 本协议族的全局诊断（bind 数、跟踪的窗口数、最近一次请求怎么处置）。 */
void woodland_protocol_toplevel_status(struct woodland_server *server,
                                       struct woodland_json *out);

/**
 * 单个视图在本协议里的对外形态（identifier、几个客户端各拿一个 handle）。
 *
 * 协议族没建起来时写 {"tracked": false}，调用方不必自己判空。
 */
void woodland_protocol_toplevel_view_status(struct woodland_view *view,
                                            struct woodland_json *out);


/* ==================== wp-security-context-v1 ==================== */

/**
 * 纯函数：协议接口是否允许沙箱客户端访问（白名单机制，对齐 labwc）。
 */
bool woodland_security_context_is_allowed(const char *interface_name);

/**
 * 纯函数：协议接口是否属于特权协议（黑名单，用于诊断和安全断言）。
 */
bool woodland_security_context_is_privileged(const char *interface_name);

/**
 * 查询指定客户端的 security context 状态（未关联或未沙箱化返回 NULL）。
 */
const struct wlr_security_context_v1_state *
woodland_protocol_client_security_context(struct woodland_server *server,
                                          const struct wl_client *client);

/**
 * 查询指定视图所属客户端的 security context 状态。
 */
const struct wlr_security_context_v1_state *
woodland_protocol_view_security_context(struct woodland_server *server,
                                        const struct woodland_view *view);

/**
 * security context 协议状态与诊断信息，供 wctl get_tree 使用。
 */
void woodland_protocol_security_context_status(struct woodland_server *server,
                                               struct woodland_json *out);
#endif
