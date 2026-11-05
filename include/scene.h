// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SCENE_H
#define WOODLAND_SCENE_H

/*
 * scene 的唯一入口。
 *
 * 实现固定是 scenefx：它顶替 wlroots 的 scene（接口名不变，仍是 wlr_scene_*），
 * 多出圆角/模糊/阴影，渲染器也必须换成 fx_renderer。两套头文件用同一个 include
 * guard，谁先被包含谁生效——混着来会拿到 A 的结构体配 B 的实现，直接踩内存。
 *
 * 所以 .c 里只 include 这个头，不要直接 include <scenefx/...> 或
 * <wlr/types/wlr_scene.h>。要"关掉特效"是运行期的事（[effects] enabled），
 * 不是换实现。
 */
#include <scenefx/types/wlr_scene.h>

#endif /* WOODLAND_SCENE_H */
