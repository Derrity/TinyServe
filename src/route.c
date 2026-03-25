#include "route.h"
#include "http_parser.h"
#include "http_response.h"
#include "server.h"
#include "auth.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INITIAL_ROUTE_CAPACITY 16

/* Trim leading and trailing whitespace in place, return pointer into str. */
static char *trim(char *str)
{
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) *end-- = '\0';
    return str;
}

/* Process escape sequences in body value: replace literal \n with newline. */
static char *unescape_body(const char *src, size_t *out_len)
{
    size_t slen = strlen(src);
    char *buf = malloc(slen + 1);
    if (!buf) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < slen; i++) {
        if (src[i] == '\\' && i + 1 < slen && src[i + 1] == 'n') {
            buf[j++] = '\n';
            i++;
        } else {
            buf[j++] = src[i];
        }
    }
    buf[j] = '\0';
    *out_len = j;
    return buf;
}

/* Append a "Key: Value\r\n" entry to an existing extra_headers string. */
static char *append_header(char *existing, const char *header_line)
{
    size_t hlen = strlen(header_line);
    size_t elen = existing ? strlen(existing) : 0;
    char *buf = realloc(existing, elen + hlen + 3);
    if (!buf) return existing;
    memcpy(buf + elen, header_line, hlen);
    buf[elen + hlen]     = '\r';
    buf[elen + hlen + 1] = '\n';
    buf[elen + hlen + 2] = '\0';
    return buf;
}

/* Initialize a route to default values. */
static void route_init(ts_route_t *r)
{
    memset(r, 0, sizeof(*r));
    r->status = 200;
}

/* Ensure the route list has room for one more entry. */
static int route_list_grow(ts_route_list_t *list)
{
    if (list->count < list->capacity) return 0;
    int new_cap = list->capacity * 2;
    ts_route_t *tmp = realloc(list->routes, (size_t)new_cap * sizeof(ts_route_t));
    if (!tmp) return -1;
    list->routes = tmp;
    list->capacity = new_cap;
    return 0;
}

/* Commit a parsed block into the route list (if it has method+path). */
static int commit_route(ts_route_list_t *list, ts_route_t *pending)
{
    if (pending->method[0] == '\0' || pending->path[0] == '\0')
        return 0; /* incomplete block, skip */

    if (route_list_grow(list) != 0) return -1;

    list->routes[list->count] = *pending;
    list->count++;
    return 0;
}

int ts_routes_load(const char *path, ts_route_list_t *list)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("Failed to open route config: %s", path);
        return -1;
    }

    list->count = 0;
    list->capacity = INITIAL_ROUTE_CAPACITY;
    list->routes = calloc((size_t)list->capacity, sizeof(ts_route_t));
    if (!list->routes) {
        fclose(fp);
        return -1;
    }

    ts_route_t cur;
    route_init(&cur);
    int in_block = 0;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        /* strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        char *trimmed = trim(line);

        /* comment */
        if (trimmed[0] == '#') continue;

        /* block separator */
        if (strcmp(trimmed, "---") == 0) {
            if (in_block) {
                if (commit_route(list, &cur) != 0) {
                    fclose(fp);
                    return -1;
                }
            }
            route_init(&cur);
            in_block = 1;
            continue;
        }

        /* skip empty lines */
        if (trimmed[0] == '\0') continue;

        if (!in_block) continue;

        /* parse key = value */
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(trimmed);
        char *val = trim(eq + 1);

        if (strcasecmp(key, "method") == 0) {
            snprintf(cur.method, sizeof(cur.method), "%s", val);
            /* uppercase */
            for (char *p = cur.method; *p; p++) *p = (char)toupper((unsigned char)*p);
        } else if (strcasecmp(key, "path") == 0) {
            snprintf(cur.path, sizeof(cur.path), "%s", val);
        } else if (strcasecmp(key, "status") == 0) {
            cur.status = atoi(val);
        } else if (strcasecmp(key, "content-type") == 0) {
            snprintf(cur.content_type, sizeof(cur.content_type), "%s", val);
        } else if (strcasecmp(key, "body") == 0) {
            free(cur.body);
            cur.body = unescape_body(val, &cur.body_len);
        } else if (strcasecmp(key, "header") == 0) {
            cur.extra_headers = append_header(cur.extra_headers, val);
        }
    }

    /* commit last block */
    if (in_block) {
        if (commit_route(list, &cur) != 0) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    LOG_INFO("Loaded %d route(s) from %s", list->count, path);
    return 0;
}

void ts_routes_free(ts_route_list_t *list)
{
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->routes[i].body);
        free(list->routes[i].extra_headers);
    }
    free(list->routes);
    list->routes = NULL;
    list->count = 0;
    list->capacity = 0;
}

void ts_route_serve(ts_client_t *client)
{
    ts_request_t *req = &client->req;
    ts_config_t  *cfg = client->config;
    int is_head = (strcmp(req->method, "HEAD") == 0);

    /* Auth check */
    if (ts_auth_enabled(cfg) && !ts_auth_check(req, cfg)) {
        int basic = ts_auth_basic_enabled(cfg);
        ts_response_send_401(client, basic, is_head);
        return;
    }

    ts_route_list_t *rl = client->routes;

    for (int i = 0; i < rl->count; i++) {
        ts_route_t *r = &rl->routes[i];

        /* For HEAD requests, match routes with GET method */
        int method_match = (strcmp(req->method, r->method) == 0) ||
                           (is_head && strcmp(r->method, "GET") == 0);
        if (!method_match) continue;
        if (strcmp(req->path, r->path) != 0) continue;

        /* Found a match */
        if (is_head) {
            ts_response_send_headers(client, r->status, r->content_type,
                                     (int64_t)r->body_len, r->extra_headers);
            ts_client_response_end(client);
        } else {
            ts_response_send(client, r->status, r->content_type,
                             r->body, r->body_len, r->extra_headers, 0);
        }
        return;
    }

    /* No matching route */
    ts_response_send_404(client, is_head);
}
