#include "auth.h"
#include "http_parser.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* ── Base64 decode table ── */
static const unsigned char b64_table[256] = {
    ['A']=0,  ['B']=1,  ['C']=2,  ['D']=3,  ['E']=4,  ['F']=5,
    ['G']=6,  ['H']=7,  ['I']=8,  ['J']=9,  ['K']=10, ['L']=11,
    ['M']=12, ['N']=13, ['O']=14, ['P']=15, ['Q']=16, ['R']=17,
    ['S']=18, ['T']=19, ['U']=20, ['V']=21, ['W']=22, ['X']=23,
    ['Y']=24, ['Z']=25,
    ['a']=26, ['b']=27, ['c']=28, ['d']=29, ['e']=30, ['f']=31,
    ['g']=32, ['h']=33, ['i']=34, ['j']=35, ['k']=36, ['l']=37,
    ['m']=38, ['n']=39, ['o']=40, ['p']=41, ['q']=42, ['r']=43,
    ['s']=44, ['t']=45, ['u']=46, ['v']=47, ['w']=48, ['x']=49,
    ['y']=50, ['z']=51,
    ['0']=52, ['1']=53, ['2']=54, ['3']=55, ['4']=56, ['5']=57,
    ['6']=58, ['7']=59, ['8']=60, ['9']=61,
    ['+']=62, ['/']=63
};

static int is_b64_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
}

/* Decode base64 `src` into `dst`. Returns decoded length or -1 on error. */
static int base64_decode(const char *src, char *dst, size_t dst_size) {
    size_t slen = strlen(src);
    size_t i = 0, o = 0;

    while (i < slen) {
        /* skip whitespace */
        if (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n') {
            i++;
            continue;
        }

        /* need at least 2 valid base64 chars */
        if (!is_b64_char((unsigned char)src[i]))
            break;

        unsigned char a = b64_table[(unsigned char)src[i++]];
        if (i >= slen || !is_b64_char((unsigned char)src[i]))
            return -1;
        unsigned char b = b64_table[(unsigned char)src[i++]];

        if (o >= dst_size) return -1;
        dst[o++] = (char)((a << 2) | (b >> 4));

        if (i >= slen || src[i] == '=') { i++; break; }
        if (!is_b64_char((unsigned char)src[i]))
            break;
        unsigned char c = b64_table[(unsigned char)src[i++]];

        if (o >= dst_size) return -1;
        dst[o++] = (char)(((b & 0x0F) << 4) | (c >> 2));

        if (i >= slen || src[i] == '=') { i++; break; }
        if (!is_b64_char((unsigned char)src[i]))
            break;
        unsigned char d = b64_table[(unsigned char)src[i++]];

        if (o >= dst_size) return -1;
        dst[o++] = (char)(((c & 0x03) << 6) | d);
    }

    if (o >= dst_size) return -1;
    dst[o] = '\0';
    return (int)o;
}

int ts_auth_enabled(const ts_config_t *cfg) {
    if (cfg->auth_user && cfg->auth_user[0] &&
        cfg->auth_pass && cfg->auth_pass[0])
        return 1;
    if (cfg->auth_header && cfg->auth_header[0] &&
        cfg->auth_value && cfg->auth_value[0])
        return 1;
    return 0;
}

int ts_auth_basic_enabled(const ts_config_t *cfg) {
    return (cfg->auth_user && cfg->auth_user[0] &&
            cfg->auth_pass && cfg->auth_pass[0]) ? 1 : 0;
}

int ts_auth_check(const ts_request_t *req, const ts_config_t *cfg) {
    if (!ts_auth_enabled(cfg))
        return 1;

    /* Check Basic Auth */
    if (ts_auth_basic_enabled(cfg)) {
        const char *auth = ts_request_header(req, "Authorization");
        if (auth && strncmp(auth, "Basic ", 6) == 0) {
            char decoded[512];
            if (base64_decode(auth + 6, decoded, sizeof(decoded)) > 0) {
                char *colon = strchr(decoded, ':');
                if (colon) {
                    *colon = '\0';
                    const char *user = decoded;
                    const char *pass = colon + 1;
                    if (strcmp(user, cfg->auth_user) == 0 &&
                        strcmp(pass, cfg->auth_pass) == 0) {
                        return 1;
                    }
                }
            }
        }
    }

    /* Check Header Auth */
    if (cfg->auth_header && cfg->auth_header[0] &&
        cfg->auth_value && cfg->auth_value[0]) {
        const char *val = ts_request_header(req, cfg->auth_header);
        if (val && strcmp(val, cfg->auth_value) == 0)
            return 1;
    }

    LOG_WARN("auth: request denied");
    return 0;
}
