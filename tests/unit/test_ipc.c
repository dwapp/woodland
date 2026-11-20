// SPDX-License-Identifier: GPL-2.0-only
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ipc.h"
#include "ipc_internal.h"
#include "trace.h"
#include "util/json.h"
#include "woodland.h"
#include "workspace.h"

static void test_next_token(void) {
    char tok[128];
    const char *p = "  plain   \"foo bar\" title=\"hello world\" escaped=\"a\\\"b\\\\c\"  ";

    p = woodland_ipc_next_token(p, tok, sizeof(tok));
    assert(p != NULL && strcmp(tok, "plain") == 0);

    p = woodland_ipc_next_token(p, tok, sizeof(tok));
    assert(p != NULL && strcmp(tok, "foo bar") == 0);

    p = woodland_ipc_next_token(p, tok, sizeof(tok));
    assert(p != NULL && strcmp(tok, "title=hello world") == 0);

    p = woodland_ipc_next_token(p, tok, sizeof(tok));
    assert(p != NULL && strcmp(tok, "escaped=a\"b\\c") == 0);

    p = woodland_ipc_next_token(p, tok, sizeof(tok));
    assert(p == NULL);
}

static void test_parse_hex_rgb(void) {
    uint32_t rgb = 0;

    assert(woodland_ipc_parse_hex_rgb("#f7f7f7", &rgb) && rgb == 0xf7f7f7u);
    assert(woodland_ipc_parse_hex_rgb("333333", &rgb) && rgb == 0x333333u);
    assert(woodland_ipc_parse_hex_rgb("#FF00AA", &rgb) && rgb == 0xff00aau);

    assert(!woodland_ipc_parse_hex_rgb(NULL, &rgb));
    assert(!woodland_ipc_parse_hex_rgb("", &rgb));
    assert(!woodland_ipc_parse_hex_rgb("#12345", &rgb));
    assert(!woodland_ipc_parse_hex_rgb("#1234567", &rgb));
    assert(!woodland_ipc_parse_hex_rgb("#zzzzzz", &rgb));
    assert(!woodland_ipc_parse_hex_rgb("12345g", &rgb));
}

static bool dispatch_cmd(struct woodland_server *server, const char *line, char *buf,
                         size_t cap) {
    struct woodland_json out;
    woodland_json_init(&out);
    bool ok = woodland_ipc_dispatch(server, line, &out);
    assert(woodland_json_ok(&out));
    assert(woodland_json_len(&out) < cap);
    snprintf(buf, cap, "%s", woodland_json_data(&out));
    woodland_json_finish(&out);
    return ok;
}

int main(void) {
    test_next_token();
    test_parse_hex_rgb();

    struct woodland_server server = {0};
    wl_list_init(&server.views);
    wl_list_init(&server.outputs);
    wl_list_init(&server.workspaces);

    char buf[32768];

    /* help 列出 pixel / screenshot / wait 且都带 fields */
    assert(dispatch_cmd(&server, "help", buf, sizeof(buf)));
    assert(strstr(buf, "\"name\":\"pixel\"") != NULL);
    assert(strstr(buf, "\"name\":\"screenshot\"") != NULL);
    assert(strstr(buf, "\"name\":\"wait\"") != NULL);
    assert(strstr(buf, "\"fields\":[{\"name\":") != NULL);

    /* pixel: 参数错误与无渲染器安全报错 */
    assert(!dispatch_cmd(&server, "pixel", buf, sizeof(buf)));
    assert(strstr(buf, "\"ok\":false") != NULL);
    assert(!dispatch_cmd(&server, "pixel 10", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "pixel 10 abc", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "pixel 10 20 30", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "pixel 10 20", buf, sizeof(buf)));
    assert(strstr(buf, "\"cmd\":\"pixel\"") != NULL);
    assert(strstr(buf, "\"ok\":false") != NULL);

    /* screenshot: 参数校验 */
    assert(!dispatch_cmd(&server, "screenshot --region bad", buf, sizeof(buf)));
    assert(strstr(buf, "\"ok\":false") != NULL);
    assert(!dispatch_cmd(&server, "screenshot --expect nope", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "screenshot --tolerance 999", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "screenshot --bogus", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "screenshot 0 0 0 10", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "screenshot", buf, sizeof(buf)));
    assert(strstr(buf, "\"cmd\":\"screenshot\"") != NULL);

    /* wait: 坏条件、未知参数、同步命中/未命中 */
    assert(!dispatch_cmd(&server, "wait", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "wait unknown_cond", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "wait view_mapped bogus=1", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "wait view_count", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "wait workspace", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "wait pixel x=1 y=2 hex=bad", buf, sizeof(buf)));
    assert(!dispatch_cmd(&server, "wait trace what=action timeout=0", buf, sizeof(buf)));

    assert(dispatch_cmd(&server, "wait view_count count=0", buf, sizeof(buf)));
    assert(strstr(buf, "\"ok\":true") != NULL);
    assert(strstr(buf, "\"matched\":true") != NULL);
    assert(strstr(buf, "\"count\":0") != NULL);

    assert(!dispatch_cmd(&server, "wait view_count count=1", buf, sizeof(buf)));
    assert(strstr(buf, "\"ok\":false") != NULL);
    assert(strstr(buf, "\"matched\":false") != NULL);

    assert(dispatch_cmd(&server, "wait focus none=1", buf, sizeof(buf)));
    assert(strstr(buf, "\"ok\":true") != NULL);
    assert(strstr(buf, "\"matched\":true") != NULL);

    struct woodland_workspace ws = {.name = "1"};
    server.active_workspace = &ws;
    assert(dispatch_cmd(&server, "wait workspace name=1", buf, sizeof(buf)));
    assert(strstr(buf, "\"matched\":true") != NULL);
    assert(!dispatch_cmd(&server, "wait workspace name=2", buf, sizeof(buf)));
    assert(strstr(buf, "\"matched\":false") != NULL);

    /* trace 条件匹配与未匹配 */
    woodland_trace_init(true);
    woodland_trace_add("action", 42, "hello world");
    assert(!dispatch_cmd(&server, "wait trace what=action detail=missing", buf, sizeof(buf)));
    assert(dispatch_cmd(&server, "wait trace what=action detail=hello obj=42", buf, sizeof(buf)));
    assert(strstr(buf, "\"ok\":true") != NULL);
    assert(strstr(buf, "\"matched\":true") != NULL);
    woodland_trace_finish();

    return 0;
}
