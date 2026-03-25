#ifndef TS_ROUTE_H
#define TS_ROUTE_H

#include "tinyserve.h"

/* Load routes from a config file.
 * Returns 0 on success, -1 on error. */
int ts_routes_load(const char *path, ts_route_list_t *list);

/* Free all routes. */
void ts_routes_free(ts_route_list_t *list);

/* Handle a request against the route table. */
void ts_route_serve(ts_client_t *client);

#endif /* TS_ROUTE_H */
