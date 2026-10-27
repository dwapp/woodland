// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <wayland-server-core.h>

#include "capture.h"
#include "woodland.h"

/* 截图模块的接线测试：只要 display 就能建好四个 manager，
 * 且 toplevel 捕获请求监听器挂上/摘下都不留下悬空链接。 */
int main(void) {
    struct woodland_server server = {0};

    server.display = wl_display_create();
    assert(server.display);
    server.event_loop = wl_display_get_event_loop(server.display);

    assert(woodland_capture_init(&server));
    assert(server.screencopy_manager);
    assert(server.image_copy_capture_manager);
    assert(server.output_capture_source_manager);
    assert(server.toplevel_capture_manager);
    assert(server.toplevel_capture_request.link.prev);
    assert(server.toplevel_capture_request.link.next);

    woodland_capture_finish(&server);
    assert(server.toplevel_capture_request.link.prev == NULL);
    assert(server.toplevel_capture_request.link.next == NULL);

    wl_display_destroy(server.display);

    printf("capture: ok\n");
    return 0;
}
