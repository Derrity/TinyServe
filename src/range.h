#ifndef TS_RANGE_H
#define TS_RANGE_H

#include "tinyserve.h"

/* Parse a Range header value (e.g., "bytes=0-1023,2048-").
 * Populates `ranges` array and sets `count`.
 * Returns:
 *   0 : success
 *  -1 : malformed range header (ignore it, serve full file)
 *  -2 : range not satisfiable (return 416) */
int ts_range_parse(const char *header, int64_t file_size,
                   ts_range_t *ranges, int max_ranges, int *count);

/* Generate a random boundary string for multipart responses. */
void ts_range_boundary(char *buf, size_t buf_size);

/* Calculate the total Content-Length for a multipart/byteranges response. */
int64_t ts_range_multipart_size(const ts_range_t *ranges, int count,
                                const char *content_type,
                                int64_t file_size,
                                const char *boundary);

#endif /* TS_RANGE_H */
