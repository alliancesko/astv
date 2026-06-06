#ifndef ASTV_CHANNEL_STORE_H
#define ASTV_CHANNEL_STORE_H

#include "ami.h"

#include <stddef.h>
#include <time.h>

#define ASTV_MAX_CHANNELS 1024

/* Normalized channel fields collected from AMI channel events. */
struct astv_channel {
    char uniqueid[64];
    char linkedid[64];
    char channel[160];
    char state[32];
    char caller_num[96];
    char caller_name[96];
    char connected_num[96];
    char connected_name[96];
    char context[96];
    char extension[96];
    char application[96];
    char bridge_id[64];
    time_t created_at;
};

/* Current channel snapshot and its lazily computed call count. */
struct channel_store {
    struct astv_channel channels[ASTV_MAX_CHANNELS];
    size_t count;
    size_t cached_call_count;
    int call_count_dirty;
};

void channel_store_init(struct channel_store *store);
void channel_store_clear(struct channel_store *store);
int channel_store_apply(struct channel_store *store,
                        const struct ami_message *message);
size_t channel_store_call_count(struct channel_store *store);

#endif
