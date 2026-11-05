// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_SCENE_H
#define WOODLAND_SCENE_H

/*
 * scene 的唯一入口。
 *
 * -Deffects=enabled 时 scenefx 换掉 wlroots 的 scene 实现：接口名不变
 * （wlr_scene_*），多了圆角/模糊/阴影，渲染器也要换成 fx_renderer。
 * 两者的头文件用同一个 include guard，所以必须二选一、且只能从这里进——
 * 谁先被包含谁生效，混着来会拿到 A 的结构体配 B 的实现，直接踩内存。
 *
 * 默认构建仍旧直出 wlroots：特效是可选特性，不是默认路径。
 */
#include "config.h"

#if WOODLAND_HAVE_EFFECTS
#include <scenefx/types/wlr_scene.h>
#else
#include <wlr/types/wlr_scene.h>
#endif

#endif /* WOODLAND_SCENE_H */
