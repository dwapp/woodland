// SPDX-License-Identifier: GPL-2.0-only
#ifndef WOODLAND_IPC_INTERNAL_H
#define WOODLAND_IPC_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct woodland_server;
struct woodland_view;
struct woodland_json;
struct woodland_ipc_wait;

/* 解析辅助：支持裸词、双引号串与 key="val with space" 形式，处理 \" 与 \\ 转义。 */
const char *woodland_ipc_next_token(const char *p, char *tok, size_t cap);
bool woodland_ipc_parse_hex_rgb(const char *s, uint32_t *rgb);

/* 像素/区域截图断言（src/ipc/inspect.c） */
bool woodland_ipc_cmd_pixel(struct woodland_server *server, const char *args,
                            struct woodland_json *out);
bool woodland_ipc_cmd_screenshot(struct woodland_server *server, const char *args,
                                 struct woodland_json *out);
bool woodland_ipc_sample_pixel(struct woodland_server *server, int lx, int ly,
                               uint8_t rgba[4], char *err, size_t err_cap);
void woodland_ipc_write_pixel_source(struct woodland_server *server, int lx, int ly,
                                     const uint8_t rgba[4], struct woodland_json *out,
                                     bool *has_expected, uint32_t *expected_rgb,
                                     const char **expected_src, bool *is_ssd_titlebar,
                                     const struct woodland_view **out_view);

/* 异步/同步条件等待（src/ipc/wait.c） */
bool woodland_ipc_wait_parse(const char *args, struct woodland_ipc_wait **out_wait,
                             char *err, size_t err_cap);
void woodland_ipc_wait_free(struct woodland_ipc_wait *wait);
int woodland_ipc_wait_timeout_ms(const struct woodland_ipc_wait *wait);
uint64_t woodland_ipc_wait_start_ms(const struct woodland_ipc_wait *wait);
bool woodland_ipc_wait_eval(struct woodland_server *server,
                            const struct woodland_ipc_wait *wait,
                            uint64_t elapsed_ms, struct woodland_json *out,
                            bool *matched);
void woodland_ipc_wait_write_timeout(const struct woodland_ipc_wait *wait,
                                     uint64_t elapsed_ms, struct woodland_json *out);
bool woodland_ipc_cmd_wait_sync(struct woodland_server *server, const char *args,
                                struct woodland_json *out);

#endif
