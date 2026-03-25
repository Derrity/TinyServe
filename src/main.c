#include "tinyserve.h"
#include "config.h"
#include "server.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    ts_config_t cfg;
    ts_config_defaults(&cfg);

    int r = ts_config_parse(argc, argv, &cfg);
    if (r == 1) return 0;
    if (r < 0) return 1;

    ts_log_init(cfg.log_level);

    return ts_server_start(&cfg);
}
