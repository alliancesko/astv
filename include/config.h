#ifndef ASTV_CONFIG_H
#define ASTV_CONFIG_H

#include <stddef.h>

#define ASTV_DEFAULT_CONFIG "/etc/astv/astv.conf"
#define ASTV_DEFAULT_HOST "127.0.0.1"
#define ASTV_DEFAULT_PORT "5038"
#define ASTV_DEFAULT_USER "monitor"

/* AMI connection settings loaded from /etc/astv or CLI overrides. */
struct astv_config {
    char host[256];
    char port[16];
    char username[128];
    char secret[256];
};

void astv_config_defaults(struct astv_config *config);
int astv_config_load(const char *path, struct astv_config *config,
                     char *error, size_t error_size);
int astv_config_save(const char *path, const struct astv_config *config,
                     char *error, size_t error_size);
int astv_config_validate(const struct astv_config *config,
                         char *error, size_t error_size);

#endif
