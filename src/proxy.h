#ifndef TS_PROXY_H
#define TS_PROXY_H

#include "tinyserve.h"
#include <uv.h>

/* Handle a new proxy connection. Called from the server on_connection callback.
 * This creates a new proxy context, accepts the client, and connects to upstream. */
void ts_proxy_handle_connection(uv_stream_t *server, ts_config_t *cfg, uv_loop_t *loop);

#endif /* TS_PROXY_H */
