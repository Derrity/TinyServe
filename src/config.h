#ifndef TS_CONFIG_H
#define TS_CONFIG_H

#include "tinyserve.h"

/* Parse command-line arguments into config.
 * Returns 0 on success, 1 if help was printed, -1 on error. */
int ts_config_parse(int argc, char **argv, ts_config_t *cfg);

/* Print usage/help. */
void ts_config_print_help(const char *prog);

/* Set defaults for config. */
void ts_config_defaults(ts_config_t *cfg);

#endif /* TS_CONFIG_H */
