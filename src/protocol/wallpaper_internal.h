// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_WALLPAPER_INTERNAL_H
#define WOODLAND_WALLPAPER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 壁纸协议的内部约定：配置模型 + 两个协议共用的枚举。
 *
 * 只在 src/protocol 内部可见——外部（ipc、server）只经 protocol.h 的
 * 几个入口访问，理由和 protocol_internal.h 一样：加协议不动 woodland.h。
 *
 * 配置的键名与数据模型**照抄 treeland**（含它那两个 typo 键
 * `desktopWallpapertype` / `lockScreenWallpapertype`），这样同一份
 * wallpaper.json 两边都能读，将来共享配置不用再做迁移。
 */

/* 源类型：取值与两个协议的 wallpaper_source_type 枚举一致 */
enum woodland_wallpaper_type {
    WOODLAND_WALLPAPER_IMAGE = 0,
    WOODLAND_WALLPAPER_VIDEO = 1,
};

/* 角色：取值与 manager 协议的 wallpaper_role 一致，可位或 */
enum woodland_wallpaper_role {
    WOODLAND_WALLPAPER_DESKTOP = 0x1,
    WOODLAND_WALLPAPER_LOCKSCREEN = 0x2,
};

/**
 * 源校验结果。
 *
 * 数值刻意等于 manager 协议的 error 枚举（invalid_source=1、
 * permission_denied=2），这样回 failed 时不用再翻译一次；`ok` 用 0，
 * 而协议里的 already_used 也是 0，所以两者**不能**混用——发送侧自己判断。
 */
enum woodland_wallpaper_source {
    WOODLAND_WALLPAPER_SOURCE_OK = 0,
    WOODLAND_WALLPAPER_SOURCE_INVALID = 1,
    WOODLAND_WALLPAPER_SOURCE_PERMISSION = 2,
};

/** 与 treeland 的 DEFAULT_WALLPAPER 一致；文件缺失时由客户端回 source_failed。 */
#define WOODLAND_WALLPAPER_DEFAULT "/usr/share/wallpapers/deepin/deepin-default.jpg"

struct woodland_wallpaper_workspace {
    int index;
    char *desktop;      // 绝对路径（可能为空串：表示"未配置"）
    int desktop_type;   // enum woodland_wallpaper_type
};

struct woodland_wallpaper_output {
    char *name;
    char *lockscreen;
    int lockscreen_type;
    struct woodland_wallpaper_workspace *workspaces;
    size_t n_workspaces;
};

struct woodland_wallpaper_config {
    struct woodland_wallpaper_output *outputs;
    size_t n_outputs;
    size_t cap_outputs;
};

void woodland_wallpaper_config_init(struct woodland_wallpaper_config *config);
void woodland_wallpaper_config_finish(struct woodland_wallpaper_config *config);

/**
 * 从 $XDG_CONFIG_HOME/woodland/wallpaper.json 读配置；失败就留空。
 *
 * 返回 false 只表示"没读到可用配置"（文件不存在、解析失败），调用方据此
 * 补默认值——不当作错误中断，壁纸缺失比合成器起不来好得多。
 */
bool woodland_wallpaper_config_load(struct woodland_wallpaper_config *config);

/** 写回同一个文件（临时文件 + rename，避免写一半留下半截 JSON）。 */
bool woodland_wallpaper_config_save(const struct woodland_wallpaper_config *config);

struct woodland_wallpaper_output *
woodland_wallpaper_config_find_output(struct woodland_wallpaper_config *config,
                                      const char *name);

/**
 * 确保某个输出有一份配置：新建时参照最后一份配置克隆（没有就全用默认壁纸），
 * 已存在时把缺的 workspace 补齐。`n_workspaces` 传当前 workspace 总数。
 *
 * 返回的非空指针在 config 生命周期内有效；`created` 非空时回填"这次是否
 * 真的增删了内容"，调用方据此决定要不要 save + 广播 add。
 */
struct woodland_wallpaper_output *
woodland_wallpaper_config_ensure_output(struct woodland_wallpaper_config *config,
                                        const char *name, size_t n_workspaces,
                                        bool *created);

/** 删除某个输出的配置（输出热拔后不再为它保留状态）。 */
void woodland_wallpaper_config_remove_output(struct woodland_wallpaper_config *config,
                                             const char *name);

/**
 * 把某个源写进配置。
 *
 * 语义对齐 treeland `WallpaperManager::setOutputWallpaper`：desktop 角色写
 * **所有** workspace（不是"当前 workspace"——上游实现就是全量写），
 * lockscreen 角色只写锁屏项。
 *
 * 返回 true 表示配置真的变了（调用方据此决定 save）。
 */
bool woodland_wallpaper_config_set_source(struct woodland_wallpaper_config *config,
                                          const char *output,
                                          enum woodland_wallpaper_role role,
                                          enum woodland_wallpaper_type type,
                                          const char *path);

/** 取某个 output 下第 index 个 workspace 的桌面壁纸（没有则返回 NULL）。 */
const char *woodland_wallpaper_config_desktop(const struct woodland_wallpaper_output *output,
                                              int index, int *type);

/**
 * 校验客户端给的源路径。
 *
 * 解码不在合成器里（在 treeland-wallpaper-factory），所以只做路径级校验：
 * 绝对路径 → 路径穿越用 realpath 归一 → 白名单前缀 → 可读 → 普通文件。
 * 白名单见 wallpaper_config.c 的 source_roots()。
 */
enum woodland_wallpaper_source woodland_wallpaper_source_check(const char *path);

#endif
