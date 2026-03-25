#include "server.h"
#include "log.h"
#include "http_parser.h"
#include "http_response.h"
#include "file_serve.h"
#include "route.h"
#include "proxy.h"
#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static uv_loop_t *loop;
static uv_tcp_t server_handle;
static ts_config_t *g_config;
static ts_route_list_t g_routes;

static uv_signal_t sigint_handle, sigterm_handle;

static ts_client_t *client_alloc(void) {
    ts_client_t *client = calloc(1, sizeof(ts_client_t));
    if (!client) return NULL;
    client->config = g_config;
    client->loop = loop;
    client->file_ctx = NULL;
    client->routes = &g_routes;
    client->pending_writes = 0;
    client->response_done = 0;
    ts_request_init(&client->req);
    return client;
}

static void client_close_cb(uv_handle_t *handle) {
    ts_client_t *client = handle->data;
    if (client->file_ctx) {
        free(client->file_ctx->read_buf);
        free(client->file_ctx);
    }
    ts_request_free(&client->req);
    free(client);
}

void ts_client_close(ts_client_t *client) {
    if (client->closing) return;
    client->closing = 1;
    /* Clean up any in-progress file context */
    if (client->file_ctx) {
        if (client->file_ctx->fd >= 0) {
            uv_fs_t close_req;
            uv_fs_close(client->loop, &close_req, client->file_ctx->fd, NULL);
            uv_fs_req_cleanup(&close_req);
        }
        free(client->file_ctx->read_buf);
        free(client->file_ctx);
        client->file_ctx = NULL;
    }
    if (!uv_is_closing((uv_handle_t *)&client->handle)) {
        uv_close((uv_handle_t *)&client->handle, client_close_cb);
    }
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void dispatch_request(ts_client_t *client);

/* ── Called when pending_writes hits 0 and response_done is set ── */
void ts_client_done(ts_client_t *client) {
    if (client->closing) return;

    /* Reset write tracking for next request */
    client->pending_writes = 0;
    client->response_done = 0;

    if (client->req.keep_alive) {
        ts_request_reset(&client->req);

        /* If there is leftover data from pipelining, try to parse it */
        if (client->req.buf_len > 0) {
            int result = ts_request_parse(&client->req, NULL, 0);
            if (result < 0) {
                /* Parse error on leftover data */
                ts_response_send(client, 400, "text/plain",
                                 "Bad Request", 11, NULL, 0);
                /* response_done is set by ts_response_send; close after write */
                client->req.keep_alive = 0;
                return;
            }
            if (result > 0) {
                /* Complete pipelined request ready — dispatch it */
                LOG_INFO("%s %s", client->req.method, client->req.raw_path);
                dispatch_request(client);
                return;
            }
            /* result == 0: need more data, fall through to uv_read_start */
        }

        uv_read_start((uv_stream_t *)&client->handle, alloc_cb, on_read);
    } else {
        ts_client_close(client);
    }
}

/* ── Mark response as fully queued ── */
void ts_client_response_end(ts_client_t *client) {
    if (client->closing) return;
    client->response_done = 1;
    if (client->pending_writes == 0) {
        ts_client_done(client);
    }
    /* else: ts_client_done will be called from write completion callback */
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    (void)suggested_size;
    buf->base = malloc(TS_READ_BUF_SIZE);
    buf->len = buf->base ? TS_READ_BUF_SIZE : 0;
}

/* Dispatch a fully-parsed request to the appropriate handler */
static void dispatch_request(ts_client_t *client) {
    /* Stop reading while request is being served */
    uv_read_stop((uv_stream_t *)&client->handle);

    if (client->config->mode == 'f') {
        ts_file_serve(client);
    } else if (client->config->mode == 's') {
        ts_route_serve(client);
    }
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    ts_client_t *client = stream->data;

    if (nread < 0) {
        if (buf->base) free(buf->base);
        ts_client_close(client);
        return;
    }

    if (nread == 0) {
        if (buf->base) free(buf->base);
        return;
    }

    int result = ts_request_parse(&client->req, buf->base, nread);
    free(buf->base);

    if (result < 0) {
        /* Parse error — send 400 and close.
         * ts_response_send sets response_done; keep_alive=0 ensures close. */
        client->req.keep_alive = 0;
        ts_response_send(client, 400, "text/plain",
                         "Bad Request", 11, NULL, 0);
        return;
    }

    if (result == 0) {
        return;  /* need more data */
    }

    LOG_INFO("%s %s", client->req.method, client->req.raw_path);
    dispatch_request(client);
}

static void on_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        LOG_ERROR("connection error: %s", uv_strerror(status));
        return;
    }

    if (g_config->mode == 'p') {
        ts_proxy_handle_connection(server, g_config, loop);
        return;
    }

    ts_client_t *client = client_alloc();
    if (!client) {
        LOG_ERROR("failed to allocate client");
        return;
    }

    uv_tcp_init(loop, &client->handle);
    client->handle.data = client;

    if (uv_accept(server, (uv_stream_t *)&client->handle) != 0) {
        LOG_ERROR("failed to accept connection");
        ts_client_close(client);
        return;
    }

    uv_read_start((uv_stream_t *)&client->handle, alloc_cb, on_read);
}

static void on_signal(uv_signal_t *handle, int signum) {
    (void)handle;
    LOG_INFO("received signal %d, shutting down...", signum);
    uv_signal_stop(&sigint_handle);
    uv_signal_stop(&sigterm_handle);
    uv_close((uv_handle_t *)&server_handle, NULL);
}

int ts_server_start(ts_config_t *cfg) {
    int r;

    g_config = cfg;
    loop = uv_default_loop();

    if (cfg->mode == 's' && cfg->config_file) {
        if (ts_routes_load(cfg->config_file, &g_routes) != 0) {
            LOG_ERROR("failed to load routes from %s", cfg->config_file);
            return -1;
        }
        LOG_INFO("loaded %d routes", g_routes.count);
    }

    uv_tcp_init(loop, &server_handle);

    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    if (strchr(cfg->bind_addr, ':')) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
        r = uv_ip6_addr(cfg->bind_addr, cfg->port, addr6);
    } else {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
        r = uv_ip4_addr(cfg->bind_addr, cfg->port, addr4);
    }
    if (r != 0) {
        LOG_ERROR("invalid address '%s': %s", cfg->bind_addr, uv_strerror(r));
        return -1;
    }

    r = uv_tcp_bind(&server_handle, (const struct sockaddr *)&addr, 0);
    if (r != 0) {
        LOG_ERROR("bind error: %s", uv_strerror(r));
        return -1;
    }

    r = uv_listen((uv_stream_t *)&server_handle, 128, on_connection);
    if (r != 0) {
        LOG_ERROR("listen error: %s", uv_strerror(r));
        return -1;
    }

    uv_signal_init(loop, &sigint_handle);
    uv_signal_init(loop, &sigterm_handle);
    uv_signal_start(&sigint_handle, on_signal, SIGINT);
    uv_signal_start(&sigterm_handle, on_signal, SIGTERM);

    LOG_INFO("tinyserve v%s started", TS_VERSION);
    LOG_INFO("mode: %c, listening on %s:%d", cfg->mode, cfg->bind_addr, cfg->port);
    if (cfg->mode == 'f') LOG_INFO("serving files from: %s", cfg->root_dir);
    if (cfg->mode == 'p') LOG_INFO("forwarding to %s:%d", cfg->target_host, cfg->target_port);

    uv_run(loop, UV_RUN_DEFAULT);

    if (cfg->mode == 's') ts_routes_free(&g_routes);
    uv_loop_close(loop);

    return 0;
}
