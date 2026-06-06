#include "channel_store.h"

#include <stdlib.h>
#include <string.h>

/* In-memory projection of channel-related AMI events. */

static void set_string(char *destination, size_t size, const char *value)
{
    if (value[0] != '\0') {
        strncpy(destination, value, size - 1);
        destination[size - 1] = '\0';
    }
}

static struct astv_channel *find_channel(struct channel_store *store,
                                         const char *uniqueid)
{
    size_t i;

    for (i = 0; i < store->count; i++) {
        if (strcmp(store->channels[i].uniqueid, uniqueid) == 0)
            return &store->channels[i];
    }
    return NULL;
}

static struct astv_channel *get_or_add(struct channel_store *store,
                                       const char *uniqueid)
{
    struct astv_channel *channel;

    if (uniqueid[0] == '\0')
        return NULL;

    channel = find_channel(store, uniqueid);
    if (channel != NULL)
        return channel;
    if (store->count == ASTV_MAX_CHANNELS)
        return NULL;

    channel = &store->channels[store->count++];
    memset(channel, 0, sizeof(*channel));
    set_string(channel->uniqueid, sizeof(channel->uniqueid), uniqueid);
    channel->created_at = time(NULL);
    return channel;
}

static void update_channel(struct astv_channel *channel,
                           const struct ami_message *message)
{
    const char *duration;

    set_string(channel->linkedid, sizeof(channel->linkedid),
               ami_get(message, "Linkedid"));
    set_string(channel->channel, sizeof(channel->channel),
               ami_get(message, "Channel"));
    set_string(channel->state, sizeof(channel->state),
               ami_get(message, "ChannelStateDesc"));
    set_string(channel->caller_num, sizeof(channel->caller_num),
               ami_get(message, "CallerIDNum"));
    set_string(channel->caller_name, sizeof(channel->caller_name),
               ami_get(message, "CallerIDName"));
    set_string(channel->connected_num, sizeof(channel->connected_num),
               ami_get(message, "ConnectedLineNum"));
    set_string(channel->connected_name, sizeof(channel->connected_name),
               ami_get(message, "ConnectedLineName"));
    set_string(channel->context, sizeof(channel->context),
               ami_get(message, "Context"));
    set_string(channel->extension, sizeof(channel->extension),
               ami_get(message, "Exten"));
    set_string(channel->application, sizeof(channel->application),
               ami_get(message, "Application"));
    /* Bridge field names differ between AMI versions and event families. */
    set_string(channel->bridge_id, sizeof(channel->bridge_id),
               ami_get(message, "BridgeUniqueid"));
    set_string(channel->bridge_id, sizeof(channel->bridge_id),
               ami_get(message, "BridgeId"));

    duration = ami_get(message, "Duration");
    if (duration[0] == '\0')
        duration = ami_get(message, "Seconds");
    if (duration[0] != '\0') {
        long seconds = strtol(duration, NULL, 10);
        if (seconds >= 0)
            channel->created_at = time(NULL) - seconds;
    }
}

static int remove_channel(struct channel_store *store, const char *uniqueid)
{
    size_t i;

    for (i = 0; i < store->count; i++) {
        if (strcmp(store->channels[i].uniqueid, uniqueid) == 0) {
            store->channels[i] = store->channels[store->count - 1];
            store->count--;
            return 1;
        }
    }
    return 0;
}

void channel_store_init(struct channel_store *store)
{
    memset(store, 0, sizeof(*store));
}

void channel_store_clear(struct channel_store *store)
{
    store->count = 0;
    store->cached_call_count = 0;
    store->call_count_dirty = 0;
}

int channel_store_apply(struct channel_store *store,
                        const struct ami_message *message)
{
    const char *event = ami_get(message, "Event");
    const char *uniqueid = ami_get(message, "Uniqueid");
    struct astv_channel *channel;

    if (strcmp(event, "Hangup") == 0) {
        int removed = remove_channel(store, uniqueid);

        if (removed)
            store->call_count_dirty = 1;
        return removed;
    }

    /* Ignore events that cannot change the channel table. */
    if (strcmp(event, "Newchannel") != 0 &&
        strcmp(event, "Newstate") != 0 &&
        strcmp(event, "NewCallerid") != 0 &&
        strcmp(event, "NewConnectedLine") != 0 &&
        strcmp(event, "CoreShowChannel") != 0 &&
        strcmp(event, "Status") != 0 &&
        strcmp(event, "BridgeEnter") != 0 &&
        strcmp(event, "BridgeLeave") != 0)
        return 0;

    channel = get_or_add(store, uniqueid);
    if (channel == NULL)
        return 0;

    update_channel(channel, message);
    if (strcmp(event, "BridgeLeave") == 0)
        channel->bridge_id[0] = '\0';
    store->call_count_dirty = 1;
    return 1;
}

static int compare_ids(const void *left, const void *right)
{
    const char *const *first = left;
    const char *const *second = right;

    return strcmp(*first, *second);
}

size_t channel_store_call_count(struct channel_store *store)
{
    const char *ids[ASTV_MAX_CHANNELS];
    size_t count = 0;
    size_t i;

    /* Recompute only after a channel event changed grouping information. */
    if (!store->call_count_dirty)
        return store->cached_call_count;
    for (i = 0; i < store->count; i++) {
        ids[i] = store->channels[i].linkedid[0] != '\0'
                     ? store->channels[i].linkedid
                     : store->channels[i].uniqueid;
    }
    /* Sorting reduces unique Linkedid counting from O(n^2) to O(n log n). */
    qsort(ids, store->count, sizeof(ids[0]), compare_ids);
    for (i = 0; i < store->count; i++) {
        if (i == 0 || strcmp(ids[i - 1], ids[i]) != 0)
            count++;
    }
    store->cached_call_count = count;
    store->call_count_dirty = 0;
    return count;
}
