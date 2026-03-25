#ifndef TS_MIME_H
#define TS_MIME_H

/* Returns MIME type string for the given file path based on extension.
 * Returns "application/octet-stream" for unknown types. */
const char *ts_mime_type(const char *path);

#endif /* TS_MIME_H */
