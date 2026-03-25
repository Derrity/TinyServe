#ifndef TS_HTTP_PARSER_H
#define TS_HTTP_PARSER_H

#include "tinyserve.h"

/* Initialize a request struct. */
void ts_request_init(ts_request_t *req);

/* Feed data to the parser. Returns:
 *   > 0 : number of bytes consumed (request complete)
 *     0 : need more data
 *    -1 : parse error */
int ts_request_parse(ts_request_t *req, const char *data, size_t len);

/* Get a header value by name (case-insensitive). Returns NULL if not found. */
const char *ts_request_header(const ts_request_t *req, const char *name);

/* Reset request for reuse (keep-alive). */
void ts_request_reset(ts_request_t *req);

/* Free any allocated memory in request. */
void ts_request_free(ts_request_t *req);

#endif /* TS_HTTP_PARSER_H */
