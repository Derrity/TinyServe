#ifndef TS_AUTH_H
#define TS_AUTH_H

#include "tinyserve.h"

/* Check if authentication is enabled. */
int ts_auth_enabled(const ts_config_t *cfg);

/* Check request against configured auth.
 * Returns 1 if authorized, 0 if not. */
int ts_auth_check(const ts_request_t *req, const ts_config_t *cfg);

/* Check if basic auth is configured. */
int ts_auth_basic_enabled(const ts_config_t *cfg);

#endif /* TS_AUTH_H */
