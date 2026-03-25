#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>

void ts_config_defaults(ts_config_t *cfg)
{
    cfg->mode        = 'f';
    cfg->bind_addr   = "127.0.0.1";
    cfg->port        = 8000;
    cfg->root_dir    = ".";
    cfg->config_file = NULL;
    cfg->auth_user   = NULL;
    cfg->auth_pass   = NULL;
    cfg->auth_header = NULL;
    cfg->auth_value  = NULL;
    cfg->target_host = NULL;
    cfg->target_port = 0;
    cfg->log_level   = TS_LOG_INFO;
}

void ts_config_print_help(const char *prog)
{
    printf("TinyServe v%s\n", TS_VERSION);
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -m <mode>    Server mode: f=file, s=stub, p=proxy (default: f)\n");
    printf("  -a <addr>    Bind address (default: 127.0.0.1)\n");
    printf("  -p <port>    Listen port (default: 8000)\n");
    printf("  -d <dir>     Root directory for file serving (default: .)\n");
    printf("  -c <file>    Config/route file path (for stub mode)\n");
    printf("  -u <user>    Basic auth username\n");
    printf("  -w <pass>    Basic auth password\n");
    printf("  -k <header>  Header auth header name\n");
    printf("  -v <value>   Header auth header value\n");
    printf("  -t <host>    Proxy target host\n");
    printf("  -q <port>    Proxy target port\n");
    printf("  -l <level>   Log level: error, warn, info (default: info)\n");
    printf("  -h           Show this help message\n");
}

/* Strictly parse a port number string.  Returns the port (1..65535)
 * or -1 on any error (negative, overflow, trailing garbage, etc.). */
static int strict_parse_port(const char *s, const char *flag_name)
{
    if (!s || *s == '\0') {
        LOG_ERROR("%s: empty value", flag_name);
        return -1;
    }

    errno = 0;
    char *end = NULL;
    long val = strtol(s, &end, 10);

    if (end == s || *end != '\0') {
        LOG_ERROR("%s: not a valid integer: '%s'", flag_name, s);
        return -1;
    }
    if (errno == ERANGE || val < 1 || val > 65535) {
        LOG_ERROR("%s: port out of range (1-65535): '%s'", flag_name, s);
        return -1;
    }
    return (int)val;
}

int ts_config_parse(int argc, char **argv, ts_config_t *cfg)
{
    int opt;

    /* Reset getopt state for repeated calls */
    optind = 1;

    while ((opt = getopt(argc, argv, "m:a:p:d:c:u:w:k:v:t:q:l:h")) != -1) {
        switch (opt) {
        case 'm':
            if (optarg[0] != 'f' && optarg[0] != 's' && optarg[0] != 'p') {
                LOG_ERROR("invalid mode '%s' (must be f, s, or p)", optarg);
                return -1;
            }
            cfg->mode = optarg[0];
            break;
        case 'a':
            cfg->bind_addr = optarg;
            break;
        case 'p': {
            int p = strict_parse_port(optarg, "-p");
            if (p < 0) return -1;
            cfg->port = p;
            break;
        }
        case 'd':
            cfg->root_dir = optarg;
            break;
        case 'c':
            cfg->config_file = optarg;
            break;
        case 'u':
            cfg->auth_user = optarg;
            break;
        case 'w':
            cfg->auth_pass = optarg;
            break;
        case 'k':
            cfg->auth_header = optarg;
            break;
        case 'v':
            cfg->auth_value = optarg;
            break;
        case 't':
            cfg->target_host = optarg;
            break;
        case 'q': {
            int q = strict_parse_port(optarg, "-q");
            if (q < 0) return -1;
            cfg->target_port = q;
            break;
        }
        case 'l':
            if (strcmp(optarg, "error") == 0)
                cfg->log_level = TS_LOG_ERROR;
            else if (strcmp(optarg, "warn") == 0)
                cfg->log_level = TS_LOG_WARN;
            else if (strcmp(optarg, "info") == 0)
                cfg->log_level = TS_LOG_INFO;
            else {
                LOG_ERROR("invalid log level '%s' (must be error, warn, or info)", optarg);
                return -1;
            }
            break;
        case 'h':
            ts_config_print_help(argv[0]);
            return 1;
        default:
            return -1;
        }
    }

    /* Validate auth pairs: must provide both or neither */
    if ((cfg->auth_user && cfg->auth_user[0]) !=
        (cfg->auth_pass && cfg->auth_pass[0])) {
        LOG_ERROR("Basic auth requires both -u <user> and -w <password>");
        return -1;
    }
    if ((cfg->auth_header && cfg->auth_header[0]) !=
        (cfg->auth_value && cfg->auth_value[0])) {
        LOG_ERROR("Header auth requires both -k <header> and -v <value>");
        return -1;
    }

    /* Proxy mode requires target host and port */
    if (cfg->mode == 'p') {
        if (!cfg->target_host || cfg->target_port == 0) {
            LOG_ERROR("proxy mode (-m p) requires -t <host> and -q <port>");
            return -1;
        }
    }

    /* Stub mode should have a config file */
    if (cfg->mode == 's' && !cfg->config_file) {
        LOG_WARN("stub mode (-m s) without -c <config file>; no routes will be loaded");
    }

    return 0;
}
