#ifndef TS_SERVER_H
#define TS_SERVER_H

#include "tinyserve.h"

/* Start the server with the given configuration.
 * This function blocks (runs the event loop). Returns 0 on clean exit. */
int ts_server_start(ts_config_t *cfg);

/* Mark the current response as fully queued.
 * If all pending writes have already completed, calls ts_client_done()
 * immediately.  Otherwise ts_client_done() fires from the last write
 * completion callback.  Use this instead of ts_client_done() for the
 * normal "response finished" path. */
void ts_client_response_end(ts_client_t *client);

/* Called internally when all writes are done and response_done is set.
 * Handles keep-alive (reset + re-read) or connection close. */
void ts_client_done(ts_client_t *client);

/* Close a client connection. Safe to call multiple times. */
void ts_client_close(ts_client_t *client);

#endif /* TS_SERVER_H */
