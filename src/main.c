#define _XOPEN_SOURCE 700

#include "ami.h"
#include "asterisk_compat.h"
#include "channel_store.h"
#include "config.h"
#include "connection.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/*
 * Main application module: AMI session state, peer snapshots, event loop,
 * keyboard navigation, and the four-panel terminal interface.
 */

/* Fixed storage limits avoid allocation while processing live AMI events. */
#define EVENT_LOG_SIZE 4096
#define MAX_PEERS 1024
#define STARTUP_TIMEOUT_MS 5000
#define EVENT_CURSOR_FOLLOW ((size_t)-1)
#define PEER_DONE_SIP (1U << 0)
#define PEER_DONE_ENDPOINTS (1U << 1)
#define PEER_DONE_CONTACTS (1U << 2)
#define PEER_DONE_IAX (1U << 3)
#define PEER_DONE_ALL (PEER_DONE_SIP | PEER_DONE_ENDPOINTS | \
                       PEER_DONE_CONTACTS | PEER_DONE_IAX)

struct options {
    struct astv_config connection;
    const char *config_path;
};

/* Event identifiers are retained for selected-channel filtering. */
struct event_entry {
    char text[256];
    char channel[160];
    char uniqueid[64];
    char dest_channel[160];
    char dest_uniqueid[64];
    char linkedid[64];
};

/* Normalized SIP, PJSIP, or IAX2 row shown in the peer panel. */
struct peer_entry {
    char driver[16];
    char name[128];
    char status[64];
    char address[128];
};

/* A complete peer refresh is built off-screen, then published atomically. */
struct peer_snapshot {
    struct peer_entry peers[MAX_PEERS];
    size_t count;
    char error[160];
    char sip_status[32];
    char pjsip_endpoint_status[32];
    char pjsip_contact_status[32];
    char iax_status[32];
    size_t sip_count;
    size_t pjsip_endpoint_count;
    size_t pjsip_contact_count;
    size_t pjsip_embedded_contact_count;
    size_t iax_count;
};

enum panel_focus {
    PANEL_CHANNELS,
    PANEL_PEERS,
    PANEL_ALL_EVENTS,
    PANEL_SELECTED_EVENTS,
    PANEL_COUNT
};

enum connection_state {
    CONNECTION_UP,
    CONNECTION_INTERRUPTED,
    CONNECTION_RECONNECTING,
    CONNECTION_FAILED
};

#define PANEL_BIT(panel) (1U << (unsigned)(panel))
#define ALL_PANEL_BITS ((1U << (unsigned)PANEL_COUNT) - 1U)

/* State owned by the single-threaded TUI event loop. */
struct app {
    struct options options;
    struct ami_parser parser;
    struct channel_store channels;
    int socket_fd;
    int authenticated;
    enum connection_state connection_state;
    int reconnect_attempts;
    time_t next_reconnect;
    time_t reconnect_connect_deadline;
    time_t reconnect_login_deadline;
    int reconnect_requested;
    struct astv_connect_job reconnect_job;
    int login_complete;
    int login_failed;
    int initial_state_requested;
    time_t version_request_deadline;
    struct astv_asterisk_version asterisk_version_info;
    char asterisk_version[64];
    char ami_version[32];
    unsigned dirty_panels;
    int full_redraw;
    int help_dirty;
    int screen_rows;
    int screen_columns;
    char login_error[256];
    struct event_entry events[EVENT_LOG_SIZE];
    size_t event_head;
    size_t event_count;
    struct peer_snapshot peer_active;
    struct peer_snapshot peer_pending;
    int peers_dirty;
    unsigned peer_generation;
    unsigned peer_done_mask;
    unsigned peer_expected_mask;
    int peer_request_inflight;
    int peer_refresh_queued;
    time_t peer_request_deadline;
    char selected_peer_driver[16];
    char selected_peer_name[128];
    char selected_uniqueid[64];
    size_t channel_cursor;
    size_t channel_offset;
    size_t peer_cursor;
    size_t peer_offset;
    size_t all_event_cursor;
    size_t selected_event_cursor;
    size_t horizontal_offset[PANEL_COUNT];
    enum panel_focus focus;
    int help_visible;
    size_t help_offset;
    int connection_lost;
    int connection_lost_dirty;
    char connection_lost_message[256];
    int peer_refresh_pending;
    time_t peer_refresh_at;
    int key_state;
    char key_parameter;
    long long escape_deadline_ms;
    struct termios saved_termios;
    int terminal_configured;
};

static volatile sig_atomic_t running = 1;

static long long monotonic_milliseconds(void);
static void prepare_reconnect(struct app *app, time_t now);

static void request_reconnect(struct app *app)
{
    app->reconnect_requested = 1;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void copy_text(char *destination, size_t size, const char *source)
{
    size_t length = strlen(source);

    if (length >= size)
        length = size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void mark_panel_dirty(struct app *app, enum panel_focus panel)
{
    app->dirty_panels |= PANEL_BIT(panel);
}

static void mark_panels_dirty(struct app *app, unsigned panels)
{
    app->dirty_panels |= panels;
}

static void request_full_redraw(struct app *app)
{
    app->full_redraw = 1;
}

/* Circular event history and selected-channel association. */

static struct event_entry *event_at(struct app *app, size_t index)
{
    return &app->events[(app->event_head + index) % EVENT_LOG_SIZE];
}

static int event_matches_selected(const struct app *app,
                                  const struct event_entry *event)
{
    size_t i;
    const char *selected_channel = "";

    if (app->selected_uniqueid[0] == '\0')
        return 0;
    for (i = 0; i < app->channels.count; i++) {
        if (strcmp(app->channels.channels[i].uniqueid,
                   app->selected_uniqueid) == 0) {
            selected_channel = app->channels.channels[i].channel;
            break;
        }
    }
    return (event->uniqueid[0] != '\0' &&
            strcmp(event->uniqueid, app->selected_uniqueid) == 0) ||
           (event->dest_uniqueid[0] != '\0' &&
            strcmp(event->dest_uniqueid, app->selected_uniqueid) == 0) ||
           (selected_channel[0] != '\0' &&
            ((event->channel[0] != '\0' &&
              strcmp(event->channel, selected_channel) == 0) ||
             (event->dest_channel[0] != '\0' &&
              strcmp(event->dest_channel, selected_channel) == 0)));
}

static struct event_entry *reserve_event(struct app *app)
{
    struct event_entry *entry;

    /* Once full, evict the oldest event and keep cursors stable. */
    if (app->event_count < EVENT_LOG_SIZE) {
        entry = event_at(app, app->event_count++);
    } else {
        struct event_entry *evicted = event_at(app, 0);
        int selected_evicted = event_matches_selected(app, evicted);

        app->event_head = (app->event_head + 1) % EVENT_LOG_SIZE;
        if (app->all_event_cursor != EVENT_CURSOR_FOLLOW &&
            app->all_event_cursor > 0)
            app->all_event_cursor--;
        if (selected_evicted &&
            app->selected_event_cursor != EVENT_CURSOR_FOLLOW &&
            app->selected_event_cursor > 0)
            app->selected_event_cursor--;
        if (selected_evicted)
            mark_panel_dirty(app, PANEL_SELECTED_EVENTS);
        entry = event_at(app, app->event_count - 1);
    }
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static void add_event_message(struct app *app,
                              const struct ami_message *message,
                              const char *format, ...)
{
    struct event_entry *entry = reserve_event(app);
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(entry->text, sizeof(entry->text), format, arguments);
    va_end(arguments);
    if (message != NULL) {
        copy_text(entry->channel, sizeof(entry->channel),
                  ami_get(message, "Channel"));
        copy_text(entry->uniqueid, sizeof(entry->uniqueid),
                  ami_get(message, "Uniqueid"));
        copy_text(entry->dest_channel, sizeof(entry->dest_channel),
                  ami_get(message, "DestChannel"));
        copy_text(entry->dest_uniqueid, sizeof(entry->dest_uniqueid),
                  ami_get(message, "DestUniqueid"));
        copy_text(entry->linkedid, sizeof(entry->linkedid),
                  ami_get(message, "Linkedid"));
    }
    mark_panel_dirty(app, PANEL_ALL_EVENTS);
    if (event_matches_selected(app, entry))
        mark_panel_dirty(app, PANEL_SELECTED_EVENTS);
}

static void add_event(struct app *app, const char *format, ...)
{
    struct event_entry *entry = reserve_event(app);
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(entry->text, sizeof(entry->text), format, arguments);
    va_end(arguments);
    mark_panel_dirty(app, PANEL_ALL_EVENTS);
}

/* AMI command transport and initial snapshot requests. */

static int send_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t written = send(fd, data, length, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int send_login(struct app *app)
{
    char message[768];
    int length;

    length = snprintf(message, sizeof(message),
                      "Action: Login\r\n"
                      "ActionID: astv-login\r\n"
                      "Username: %s\r\n"
                      "Secret: %s\r\n"
                      "Events: on\r\n\r\n",
                      app->options.connection.username,
                      app->options.connection.secret);
    if (length < 0 || (size_t)length >= sizeof(message))
        return -1;
    return send_all(app->socket_fd, message, (size_t)length);
}

static int request_core_settings(struct app *app)
{
    static const char request[] =
        "Action: CoreSettings\r\n"
        "ActionID: astv-core-settings\r\n\r\n";

    app->version_request_deadline = time(NULL) + 2;
    return send_all(app->socket_fd, request, sizeof(request) - 1);
}

static int request_channels(struct app *app)
{
    static const char core_request[] =
        "Action: CoreShowChannels\r\n"
        "ActionID: astv-channels\r\n\r\n";
    static const char status_request[] =
        "Action: Status\r\n"
        "ActionID: astv-status\r\n"
        "AllVariables: false\r\n\r\n";

    channel_store_clear(&app->channels);
    mark_panels_dirty(app, PANEL_BIT(PANEL_CHANNELS) |
                           PANEL_BIT(PANEL_SELECTED_EVENTS));
    if (send_all(app->socket_fd, core_request,
                 sizeof(core_request) - 1) < 0)
        return -1;
    if (astv_asterisk_supports_status_snapshot(
            &app->asterisk_version_info))
        return send_all(app->socket_fd, status_request,
                        sizeof(status_request) - 1);
    return 0;
}

static int request_registration_statuses(struct app *app)
{
    static const char request[] =
        "Action: PJSIPShowRegistrationInboundContactStatuses\r\n"
        "ActionID: astv-pjsip-inbound\r\n\r\n"
        "Action: PJSIPShowRegistrationsOutbound\r\n"
        "ActionID: astv-pjsip-outbound\r\n\r\n";

    return send_all(app->socket_fd, request, sizeof(request) - 1);
}

static int request_peers(struct app *app)
{
    char request[1024];
    char contacts_request[160] = "";
    int length;
    int contacts_supported =
        astv_asterisk_supports_pjsip_contacts(
            &app->asterisk_version_info);

    /* Coalesce channel-event bursts into one follow-up refresh. */
    if (app->peer_request_inflight) {
        app->peer_refresh_queued = 1;
        return 0;
    }
    memset(&app->peer_pending, 0, sizeof(app->peer_pending));
    copy_text(app->peer_pending.sip_status,
              sizeof(app->peer_pending.sip_status), "pending");
    copy_text(app->peer_pending.pjsip_endpoint_status,
              sizeof(app->peer_pending.pjsip_endpoint_status), "pending");
    copy_text(app->peer_pending.pjsip_contact_status,
              sizeof(app->peer_pending.pjsip_contact_status),
              contacts_supported ? "pending" : "unsupported");
    copy_text(app->peer_pending.iax_status,
              sizeof(app->peer_pending.iax_status), "pending");
    /* Generation-specific ActionIDs reject delayed list responses. */
    app->peer_generation++;
    if (app->peer_generation == 0)
        app->peer_generation = 1;
    app->peer_done_mask = contacts_supported ? 0U : PEER_DONE_CONTACTS;
    app->peer_expected_mask = PEER_DONE_ALL;
    app->peer_request_inflight = 1;
    app->peer_request_deadline = time(NULL) + 3;
    if (contacts_supported) {
        int contacts_length = snprintf(
            contacts_request, sizeof(contacts_request),
            "Action: PJSIPShowContacts\r\n"
            "ActionID: astv-contacts-%u\r\n\r\n",
            app->peer_generation);

        if (contacts_length < 0 ||
            (size_t)contacts_length >= sizeof(contacts_request)) {
            app->peer_request_inflight = 0;
            return -1;
        }
    }
    length = snprintf(
        request, sizeof(request),
        "Action: SIPpeers\r\n"
        "ActionID: astv-sip-%u\r\n\r\n"
        "Action: PJSIPShowEndpoints\r\n"
        "ActionID: astv-endpoints-%u\r\n\r\n"
        "Action: IAXpeers\r\n"
        "ActionID: astv-iax-%u\r\n\r\n"
        "%s",
        app->peer_generation, app->peer_generation,
        app->peer_generation,
        contacts_request);
    if (length < 0 || (size_t)length >= sizeof(request)) {
        app->peer_request_inflight = 0;
        return -1;
    }
    if (send_all(app->socket_fd, request, (size_t)length) < 0) {
        app->peer_request_inflight = 0;
        return -1;
    }
    return 0;
}

static int request_initial_state(struct app *app)
{
    if (app->initial_state_requested)
        return 0;
    app->initial_state_requested = 1;
    if (request_channels(app) < 0 ||
        request_registration_statuses(app) < 0 ||
        request_peers(app) < 0)
        return -1;
    return 0;
}

/* Peer snapshot construction and unsolicited live status updates. */

static struct peer_entry *get_snapshot_peer(struct peer_snapshot *snapshot,
                                            const char *driver,
                                            const char *name)
{
    struct peer_entry *peer;
    size_t i;

    if (name[0] == '\0')
        return NULL;
    for (i = 0; i < snapshot->count; i++) {
        if (strcmp(snapshot->peers[i].driver, driver) == 0 &&
            strcmp(snapshot->peers[i].name, name) == 0)
            return &snapshot->peers[i];
    }
    if (snapshot->count >= MAX_PEERS)
        return NULL;

    peer = &snapshot->peers[snapshot->count++];
    memset(peer, 0, sizeof(*peer));
    copy_text(peer->driver, sizeof(peer->driver), driver);
    copy_text(peer->name, sizeof(peer->name), name);
    return peer;
}

static struct peer_entry *get_peer(struct app *app, const char *driver,
                                   const char *name)
{
    struct peer_entry *peer =
        get_snapshot_peer(&app->peer_active, driver, name);

    if (peer == NULL)
        return NULL;
    app->peers_dirty = 1;
    mark_panel_dirty(app, PANEL_PEERS);
    return peer;
}

static const char *peer_name_without_driver(const char *peer)
{
    const char *slash = strchr(peer, '/');

    return slash != NULL ? slash + 1 : peer;
}

static const char *channel_technology_name(const char *channel_type)
{
    if (strcmp(channel_type, "PJSIP") == 0)
        return "chan_pjsip";
    if (strcmp(channel_type, "IAX2") == 0)
        return "chan_iax2";
    return "chan_sip";
}

static int is_channel_suffix(const char *text)
{
    size_t i;

    if (strlen(text) != 8)
        return 0;
    for (i = 0; i < 8; i++) {
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f') ||
              (text[i] >= 'A' && text[i] <= 'F')))
            return 0;
    }
    return 1;
}

static int is_decimal_suffix(const char *text)
{
    if (*text == '\0')
        return 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9')
            return 0;
        text++;
    }
    return 1;
}

static void set_peer_in_use_from_channel(struct app *app,
                                         const char *channel)
{
    char driver[16];
    char name[128];
    const char *slash;
    char *suffix;
    size_t driver_length;
    struct peer_entry *peer;

    slash = strchr(channel, '/');
    if (slash == NULL)
        return;
    driver_length = (size_t)(slash - channel);
    if (driver_length == 0 || driver_length >= sizeof(driver))
        return;
    memcpy(driver, channel, driver_length);
    driver[driver_length] = '\0';
    if (strcmp(driver, "SIP") != 0 &&
        strcmp(driver, "PJSIP") != 0 &&
        strcmp(driver, "IAX2") != 0)
        return;

    copy_text(name, sizeof(name), slash + 1);
    /* Strip Asterisk's per-call suffix to recover the configured peer name. */
    suffix = strrchr(name, '-');
    if (suffix != NULL &&
        (is_channel_suffix(suffix + 1) ||
         (strcmp(driver, "IAX2") == 0 &&
          is_decimal_suffix(suffix + 1))))
        *suffix = '\0';
    peer = get_peer(app, driver, name);
    if (peer != NULL) {
        copy_text(peer->status, sizeof(peer->status), "In use");
        mark_panel_dirty(app, PANEL_PEERS);
    }
}

static size_t ami_list_items(const struct ami_message *message,
                             size_t fallback)
{
    const char *text = ami_get(message, "ListItems");
    char *end;
    unsigned long parsed;

    if (text[0] == '\0')
        return fallback;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > SIZE_MAX)
        return fallback;
    return (size_t)parsed;
}

static int peer_action_matches(const struct app *app, const char *action_id,
                               const char *kind)
{
    char expected[64];

    snprintf(expected, sizeof(expected), "astv-%s-%u", kind,
             app->peer_generation);
    return strcmp(action_id, expected) == 0;
}

static int peer_source_succeeded(const char *status)
{
    return strcmp(status, "accepted") == 0;
}

static void preserve_active_peers(struct app *app, const char *driver)
{
    size_t i;

    for (i = 0; i < app->peer_active.count; i++) {
        const struct peer_entry *active = &app->peer_active.peers[i];
        struct peer_entry *pending;

        if (strcmp(active->driver, driver) != 0)
            continue;
        pending = get_snapshot_peer(&app->peer_pending, active->driver,
                                    active->name);
        if (pending != NULL)
            *pending = *active;
    }
}

static void publish_peer_snapshot(struct app *app)
{
    int pjsip_succeeded =
        peer_source_succeeded(
            app->peer_pending.pjsip_endpoint_status) ||
        peer_source_succeeded(
            app->peer_pending.pjsip_contact_status);

    /*
     * Preserve last-known rows only for failed technologies. Successful
     * sources replace old rows completely, so deletions remain visible.
     */
    if (!peer_source_succeeded(app->peer_pending.sip_status)) {
        preserve_active_peers(app, "SIP");
        app->peer_pending.sip_count = app->peer_active.sip_count;
    }
    if (!peer_source_succeeded(app->peer_pending.iax_status)) {
        preserve_active_peers(app, "IAX2");
        app->peer_pending.iax_count = app->peer_active.iax_count;
    }
    if (!pjsip_succeeded) {
        preserve_active_peers(app, "PJSIP");
        app->peer_pending.pjsip_endpoint_count =
            app->peer_active.pjsip_endpoint_count;
        app->peer_pending.pjsip_contact_count =
            app->peer_active.pjsip_contact_count;
        app->peer_pending.pjsip_embedded_contact_count =
            app->peer_active.pjsip_embedded_contact_count;
    }
    app->peer_active = app->peer_pending;
    app->peer_request_inflight = 0;
    app->peer_request_deadline = 0;
    app->peers_dirty = 1;
    mark_panel_dirty(app, PANEL_PEERS);
    if (app->peer_refresh_queued) {
        app->peer_refresh_queued = 0;
        app->peer_refresh_pending = 1;
        app->peer_refresh_at = time(NULL);
    }
}

static void finish_peer_source(struct app *app, unsigned source)
{
    app->peer_done_mask |= source;
    if ((app->peer_done_mask & app->peer_expected_mask) ==
        app->peer_expected_mask)
        publish_peer_snapshot(app);
}

static void timeout_peer_snapshot(struct app *app)
{
    /* Publish useful partial data instead of waiting indefinitely. */
    if (!(app->peer_done_mask & PEER_DONE_SIP))
        copy_text(app->peer_pending.sip_status,
                  sizeof(app->peer_pending.sip_status), "error");
    if (!(app->peer_done_mask & PEER_DONE_ENDPOINTS))
        copy_text(app->peer_pending.pjsip_endpoint_status,
                  sizeof(app->peer_pending.pjsip_endpoint_status),
                  "error");
    if (!(app->peer_done_mask & PEER_DONE_CONTACTS))
        copy_text(app->peer_pending.pjsip_contact_status,
                  sizeof(app->peer_pending.pjsip_contact_status),
                  "error");
    if (!(app->peer_done_mask & PEER_DONE_IAX))
        copy_text(app->peer_pending.iax_status,
                  sizeof(app->peer_pending.iax_status), "error");
    copy_text(app->peer_pending.error,
              sizeof(app->peer_pending.error),
              "peer snapshot timed out; partial data shown");
    publish_peer_snapshot(app);
}

static void update_peer_store(struct app *app,
                              const struct ami_message *message)
{
    const char *event = ami_get(message, "Event");
    const char *action_id = ami_get(message, "ActionID");
    const char *response = ami_get(message, "Response");
    struct peer_snapshot *snapshot = &app->peer_pending;
    struct peer_entry *peer = NULL;
    unsigned response_source = 0;
    char *request_status = NULL;
    size_t request_status_size = 0;

    /* Consume the accept/reject response for each requested list first. */
    if (app->peer_request_inflight && response[0] != '\0') {
        if (peer_action_matches(app, action_id, "sip")) {
            request_status = snapshot->sip_status;
            request_status_size = sizeof(snapshot->sip_status);
            response_source = PEER_DONE_SIP;
        } else if (peer_action_matches(app, action_id, "endpoints")) {
            request_status = snapshot->pjsip_endpoint_status;
            request_status_size =
                sizeof(snapshot->pjsip_endpoint_status);
            response_source = PEER_DONE_ENDPOINTS;
        } else if (peer_action_matches(app, action_id, "contacts")) {
            request_status = snapshot->pjsip_contact_status;
            request_status_size =
                sizeof(snapshot->pjsip_contact_status);
            response_source = PEER_DONE_CONTACTS;
        } else if (peer_action_matches(app, action_id, "iax")) {
            request_status = snapshot->iax_status;
            request_status_size = sizeof(snapshot->iax_status);
            response_source = PEER_DONE_IAX;
        }
        if (request_status != NULL) {
            int success = strcmp(response, "Success") == 0;

            copy_text(request_status, request_status_size,
                      success ? "accepted" : "error");
            if (!success) {
                snprintf(snapshot->error, sizeof(snapshot->error),
                         "%s", ami_get(message, "Message"));
                finish_peer_source(app, response_source);
            }
            return;
        }
    }

    if (app->peer_request_inflight &&
        (strcmp(event, "Peerlist") == 0 ||
         strcmp(event, "PeerEntry") == 0)) {
        const char *driver = ami_get(message, "Channeltype");
        const char *name = ami_get(message, "ObjectName");
        int iax = peer_action_matches(app, action_id, "iax") ||
                  strcmp(driver, "IAX2") == 0;

        if (action_id[0] != '\0' &&
            !peer_action_matches(app, action_id, "sip") &&
            !peer_action_matches(app, action_id, "iax"))
            return;
        if (name[0] == '\0')
            name = ami_get(message, "Peer");
        if (iax) {
            driver = "IAX2";
            snapshot->iax_count++;
        } else {
            driver = strcmp(driver, "PJSIP") == 0 ? "PJSIP" : "SIP";
            snapshot->sip_count++;
        }
        peer = get_snapshot_peer(snapshot, driver, name);
        if (peer != NULL) {
            const char *address = ami_get(message, "IPaddress");

            if (address[0] == '\0')
                address = ami_get(message, "Address");
            copy_text(peer->status, sizeof(peer->status),
                      ami_get(message, "Status"));
            copy_text(peer->address, sizeof(peer->address),
                      address);
        }
        return;
    }
    if (app->peer_request_inflight &&
        strcmp(event, "PeerlistComplete") == 0) {
        if (peer_action_matches(app, action_id, "iax"))
            finish_peer_source(app, PEER_DONE_IAX);
        else if (peer_action_matches(app, action_id, "sip"))
            finish_peer_source(app, PEER_DONE_SIP);
        return;
    }
    if (app->peer_request_inflight &&
        strcmp(event, "EndpointList") == 0) {
        const char *contacts = ami_get(message, "Contacts");

        if (action_id[0] != '\0' &&
            !peer_action_matches(app, action_id, "endpoints"))
            return;
        copy_text(snapshot->pjsip_endpoint_status,
                  sizeof(snapshot->pjsip_endpoint_status), "accepted");
        snapshot->pjsip_endpoint_count++;
        snapshot->pjsip_embedded_contact_count +=
            astv_pjsip_contact_count(contacts);
        peer = get_snapshot_peer(snapshot, "PJSIP",
                                 ami_get(message, "ObjectName"));
        if (peer != NULL) {
            copy_text(peer->status, sizeof(peer->status),
                      ami_get(message, "DeviceState"));
            copy_text(peer->address, sizeof(peer->address),
                      contacts);
        }
        return;
    }
    if (app->peer_request_inflight &&
        strcmp(event, "EndpointListComplete") == 0) {
        if (action_id[0] == '\0' ||
            peer_action_matches(app, action_id, "endpoints")) {
            copy_text(snapshot->pjsip_endpoint_status,
                      sizeof(snapshot->pjsip_endpoint_status), "accepted");
            snapshot->pjsip_endpoint_count =
                ami_list_items(message,
                               snapshot->pjsip_endpoint_count);
            finish_peer_source(app, PEER_DONE_ENDPOINTS);
        }
        return;
    }
    if (app->peer_request_inflight &&
        strcmp(event, "ContactList") == 0) {
        const char *name = ami_get(message, "Endpoint");

        if (action_id[0] != '\0' &&
            !peer_action_matches(app, action_id, "contacts"))
            return;
        copy_text(snapshot->pjsip_contact_status,
                  sizeof(snapshot->pjsip_contact_status), "accepted");
        snapshot->pjsip_contact_count++;
        if (name[0] == '\0')
            name = ami_get(message, "ObjectName");
        peer = get_snapshot_peer(snapshot, "PJSIP", name);
        if (peer != NULL) {
            copy_text(peer->status, sizeof(peer->status),
                      ami_get(message, "Status"));
            copy_text(peer->address, sizeof(peer->address),
                      ami_get(message, "Uri"));
        }
        return;
    }
    if (app->peer_request_inflight &&
        strcmp(event, "ContactListComplete") == 0) {
        if (action_id[0] == '\0' ||
            peer_action_matches(app, action_id, "contacts")) {
            copy_text(snapshot->pjsip_contact_status,
                      sizeof(snapshot->pjsip_contact_status), "accepted");
            snapshot->pjsip_contact_count =
                ami_list_items(message,
                               snapshot->pjsip_contact_count);
            finish_peer_source(app, PEER_DONE_CONTACTS);
        }
        return;
    }

    /* Unsolicited status events update the already visible snapshot. */
    if (strcmp(event, "ContactStatus") == 0 ||
        strcmp(event, "ContactStatusDetail") == 0) {
        const char *name = ami_get(message, "EndpointName");

        if (name[0] == '\0')
            name = ami_get(message, "AOR");
        peer = get_peer(app, "PJSIP", name);
        if (peer != NULL) {
            const char *status = ami_get(message, "ContactStatus");

            if (status[0] == '\0')
                status = ami_get(message, "Status");
            copy_text(peer->status, sizeof(peer->status), status);
            copy_text(peer->address, sizeof(peer->address),
                      ami_get(message, "URI"));
        }
    } else if (strcmp(event, "PeerStatus") == 0) {
        const char *channel_type = ami_get(message, "ChannelType");
        const char *name = peer_name_without_driver(
            ami_get(message, "Peer"));

        if (strcmp(channel_type, "PJSIP") == 0)
            channel_type = "PJSIP";
        else if (strcmp(channel_type, "IAX2") == 0)
            channel_type = "IAX2";
        else
            channel_type = "SIP";
        peer = get_peer(app, channel_type, name);
        if (peer != NULL)
            copy_text(peer->status, sizeof(peer->status),
                      ami_get(message, "PeerStatus"));
    } else if (strcmp(event, "DeviceStateChange") == 0) {
        const char *device = ami_get(message, "Device");
        const char *slash = strchr(device, '/');
        char driver[16];

        if (slash != NULL) {
            size_t driver_length = (size_t)(slash - device);

            if (driver_length >= sizeof(driver))
                driver_length = sizeof(driver) - 1;
            memcpy(driver, device, driver_length);
            driver[driver_length] = '\0';
            if (strcmp(driver, "SIP") == 0 ||
                strcmp(driver, "PJSIP") == 0 ||
                strcmp(driver, "IAX2") == 0) {
                peer = get_peer(app, driver, slash + 1);
                if (peer != NULL)
                    copy_text(peer->status, sizeof(peer->status),
                              ami_get(message, "State"));
            }
        }
    }
    if (peer != NULL)
        mark_panel_dirty(app, PANEL_PEERS);
}

/* Convert selected AMI events into compact, human-readable log lines. */

static void log_ami_event(struct app *app,
                          const struct ami_message *message)
{
    const char *event = ami_get(message, "Event");
    const char *channel = ami_get(message, "Channel");

    if (event[0] == '\0' ||
        strcmp(event, "CoreShowChannel") == 0 ||
        strcmp(event, "CoreShowChannelsComplete") == 0 ||
        strcmp(event, "Status") == 0 ||
        strcmp(event, "StatusComplete") == 0)
        return;
    if (strcmp(event, "RTCPReceived") == 0 ||
        strcmp(event, "RTCPSent") == 0)
        return;

    if (strcmp(event, "ContactStatus") == 0) {
        const char *endpoint = ami_get(message, "EndpointName");
        const char *aor = ami_get(message, "AOR");
        const char *status = ami_get(message, "ContactStatus");
        const char *uri = ami_get(message, "URI");

        add_event_message(app, message, "chan_pjsip %s: %s%s%s",
                          endpoint[0] != '\0' ? endpoint : aor,
                          status[0] != '\0' ? status : "Unknown",
                          uri[0] != '\0' ? " " : "",
                          uri);
    } else if (strcmp(event, "ContactStatusDetail") == 0) {
        const char *endpoint = ami_get(message, "EndpointName");
        const char *aor = ami_get(message, "AOR");
        const char *status = ami_get(message, "Status");
        const char *uri = ami_get(message, "URI");

        add_event_message(app, message, "chan_pjsip %s: %s%s%s",
                          endpoint[0] != '\0' ? endpoint : aor,
                          status[0] != '\0' ? status : "Unknown",
                          uri[0] != '\0' ? " " : "",
                          uri);
    } else if (strcmp(event, "OutboundRegistrationDetail") == 0) {
        const char *name = ami_get(message, "ObjectName");
        const char *server_uri = ami_get(message, "ServerUri");
        const char *status = ami_get(message, "Status");

        add_event_message(app, message,
                          "chan_pjsip registry %s: %s%s%s",
                          name[0] != '\0' ? name : "unknown",
                          status[0] != '\0' ? status : "Unknown",
                          server_uri[0] != '\0' ? " " : "",
                          server_uri);
    } else if (strcmp(event, "PeerStatus") == 0) {
        const char *channel_type = ami_get(message, "ChannelType");
        const char *peer = ami_get(message, "Peer");
        const char *status = ami_get(message, "PeerStatus");
        const char *cause = ami_get(message, "Cause");

        add_event_message(app, message, "%s peer %s: %s%s%s",
                          channel_technology_name(channel_type),
                          peer[0] != '\0' ? peer : "unknown",
                          status[0] != '\0' ? status : "Unknown",
                          cause[0] != '\0' ? " - " : "",
                          cause);
    } else if (strcmp(event, "Registry") == 0) {
        const char *channel_type = ami_get(message, "ChannelType");
        const char *username = ami_get(message, "Username");
        const char *domain = ami_get(message, "Domain");
        const char *status = ami_get(message, "Status");
        const char *cause = ami_get(message, "Cause");

        add_event_message(app, message,
                          "%s registry %s%s%s: %s%s%s",
                          channel_technology_name(channel_type),
                          username[0] != '\0' ? username : "unknown",
                          domain[0] != '\0' ? "@" : "",
                          domain,
                          status[0] != '\0' ? status : "Unknown",
                          cause[0] != '\0' ? " - " : "",
                          cause);
    } else if (channel[0] != '\0') {
        add_event_message(app, message, "%s: %s", event, channel);
    } else if (strcmp(event, "FullyBooted") == 0 ||
               strcmp(event, "Shutdown") == 0) {
        add_event_message(app, message, "%s", event);
    } else {
        const char *object = ami_get(message, "ObjectName");
        const char *status = ami_get(message, "Status");
        const char *message_text = ami_get(message, "Message");

        add_event_message(app, message, "%s%s%s%s%s%s%s",
                          event,
                          object[0] != '\0' ? ": " : "",
                          object,
                          status[0] != '\0' ? " " : "",
                          status,
                          message_text[0] != '\0' ? " - " : "",
                          message_text);
    }
}

static void handle_message(const struct ami_message *message, void *data)
{
    struct app *app = data;
    const char *response = ami_get(message, "Response");
    const char *action_id = ami_get(message, "ActionID");
    const char *event = ami_get(message, "Event");

    if (message->truncated)
        add_event(app,
                  "AMI message had more than %d fields; extra fields ignored",
                  AMI_MAX_FIELDS);

    /* Authentication and version discovery gate the initial snapshots. */
    if (!app->authenticated && strcmp(action_id, "astv-login") == 0) {
        app->login_complete = 1;
        if (strcmp(response, "Success") == 0) {
            app->authenticated = 1;
            app->login_failed = 0;
            app->connection_state = CONNECTION_UP;
            app->reconnect_attempts = 0;
            app->reconnect_login_deadline = 0;
            mark_panel_dirty(app, PANEL_CHANNELS);
            add_event(app, "AMI authentication successful");
            if (request_core_settings(app) < 0) {
                add_event(app, "failed to request Asterisk version");
                request_reconnect(app);
            }
        } else {
            const char *message_text = ami_get(message, "Message");

            app->login_failed = 1;
            snprintf(app->login_error, sizeof(app->login_error),
                     "AMI authentication failed: %s",
                     message_text[0] != '\0' ? message_text
                                             : "access denied");
        }
        return;
    }

    if (strcmp(action_id, "astv-core-settings") == 0 &&
        response[0] != '\0') {
        const char *asterisk_version = ami_get(message,
                                                "AsteriskVersion");
        const char *ami_version = ami_get(message, "AMIversion");

        if (strcmp(response, "Success") == 0) {
            copy_text(app->asterisk_version,
                      sizeof(app->asterisk_version),
                      asterisk_version);
            copy_text(app->ami_version, sizeof(app->ami_version),
                      ami_version);
            if (astv_asterisk_version_parse(
                    app->asterisk_version,
                    &app->asterisk_version_info) < 0)
                memset(&app->asterisk_version_info, 0,
                       sizeof(app->asterisk_version_info));
            add_event(app, "Asterisk %s, AMI %s",
                      app->asterisk_version[0] != '\0'
                          ? app->asterisk_version
                          : "unknown",
                      app->ami_version[0] != '\0'
                          ? app->ami_version
                          : "unknown");
        } else {
            add_event(app, "Asterisk version detection failed: %s",
                      ami_get(message, "Message"));
        }
        if (request_initial_state(app) < 0) {
            add_event(app, "failed to request initial state");
            request_reconnect(app);
        }
        return;
    }

    /* Channel topology changes schedule a debounced peer refresh. */
    if (strcmp(event, "Newchannel") == 0 ||
        strcmp(event, "Newstate") == 0 ||
        strcmp(event, "DialBegin") == 0 ||
        strcmp(event, "DialEnd") == 0 ||
        strcmp(event, "BridgeEnter") == 0 ||
        strcmp(event, "BridgeLeave") == 0 ||
        strcmp(event, "Hangup") == 0) {
        app->peer_refresh_pending = 1;
        app->peer_refresh_at = time(NULL) + 1;
    }

    if (strcmp(event, "Newchannel") == 0 ||
        strcmp(event, "DialBegin") == 0 ||
        strcmp(event, "BridgeEnter") == 0) {
        set_peer_in_use_from_channel(app, ami_get(message, "Channel"));
        set_peer_in_use_from_channel(app, ami_get(message, "DestChannel"));
    }

    update_peer_store(app, message);
    if (channel_store_apply(&app->channels, message))
        mark_panels_dirty(app, PANEL_BIT(PANEL_CHANNELS) |
                               PANEL_BIT(PANEL_SELECTED_EVENTS));
    log_ami_event(app, message);
}

/* Initial authentication and reconnect state transitions. */

static int startup_connect(struct app *app, char *error_text,
                           size_t error_size)
{
    struct pollfd descriptor;
    struct timespec started;
    struct timespec pause = {0, 20 * 1000 * 1000};
    long long connect_deadline;

    if (astv_connect_job_start(
            &app->reconnect_job,
            app->options.connection.host,
            app->options.connection.port,
            STARTUP_TIMEOUT_MS) < 0) {
        copy_text(error_text, error_size, app->reconnect_job.error);
        return -1;
    }
    connect_deadline = monotonic_milliseconds() + STARTUP_TIMEOUT_MS;
    while (!astv_connect_job_finished(&app->reconnect_job) &&
           monotonic_milliseconds() < connect_deadline)
        nanosleep(&pause, NULL);
    if (!astv_connect_job_finished(&app->reconnect_job)) {
        astv_connect_job_cancel(&app->reconnect_job);
        snprintf(error_text, error_size,
                 "timeout connecting to %.128s:%.15s",
                 app->options.connection.host,
                 app->options.connection.port);
        return -1;
    }
    astv_connect_job_finish(&app->reconnect_job);
    app->socket_fd = app->reconnect_job.fd;
    if (app->socket_fd < 0) {
        copy_text(error_text, error_size, app->reconnect_job.error);
        return -1;
    }

    ami_parser_init(&app->parser);
    app->login_complete = 0;
    app->login_failed = 0;
    app->reconnect_requested = 0;
    app->reconnect_login_deadline = 0;
    app->login_error[0] = '\0';
    if (send_login(app) < 0) {
        snprintf(error_text, error_size,
                 "cannot send AMI login request: %s", strerror(errno));
        close(app->socket_fd);
        app->socket_fd = -1;
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &started) < 0) {
        snprintf(error_text, error_size, "cannot start connection timer: %s",
                 strerror(errno));
        close(app->socket_fd);
        app->socket_fd = -1;
        return -1;
    }

    descriptor.fd = app->socket_fd;
    descriptor.events = POLLIN;
    /* Startup stays outside the alternate screen and fails normally. */
    while (!app->login_complete) {
        struct timespec now;
        long elapsed_ms;
        int remaining_ms;
        int poll_result;

        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            break;
        elapsed_ms = (now.tv_sec - started.tv_sec) * 1000L +
                     (now.tv_nsec - started.tv_nsec) / 1000000L;
        remaining_ms = STARTUP_TIMEOUT_MS - (int)elapsed_ms;
        if (remaining_ms <= 0)
            break;

        poll_result = poll(&descriptor, 1, remaining_ms);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            snprintf(error_text, error_size,
                     "error while waiting for AMI login: %s",
                     strerror(errno));
            close(app->socket_fd);
            app->socket_fd = -1;
            return -1;
        }
        if (poll_result == 0)
            break;
        if (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) {
            char buffer[8192];
            ssize_t received =
                recv(app->socket_fd, buffer, sizeof(buffer), 0);

            if (received <= 0) {
                snprintf(error_text, error_size,
                         "AMI server closed the connection during login");
                close(app->socket_fd);
                app->socket_fd = -1;
                return -1;
            }
            if (ami_parser_feed(&app->parser, buffer, (size_t)received,
                                handle_message, app) < 0) {
                snprintf(error_text, error_size,
                         "AMI login response exceeded parser buffer");
                close(app->socket_fd);
                app->socket_fd = -1;
                return -1;
            }
        }
    }

    if (!app->login_complete) {
        snprintf(error_text, error_size,
                 "timeout waiting for AMI login response from %.128s:%.15s",
                 app->options.connection.host,
                 app->options.connection.port);
        close(app->socket_fd);
        app->socket_fd = -1;
        return -1;
    }
    if (app->login_failed) {
        copy_text(error_text, error_size, app->login_error);
        close(app->socket_fd);
        app->socket_fd = -1;
        return -1;
    }
    return 0;
}

static void mark_connection_lost(struct app *app, const char *format, ...)
{
    va_list arguments;

    if (app->socket_fd >= 0)
        close(app->socket_fd);
    app->socket_fd = -1;
    app->authenticated = 0;
    app->peer_refresh_pending = 0;
    app->peer_request_inflight = 0;
    app->peer_refresh_queued = 0;
    app->connection_lost = 1;
    app->connection_state = CONNECTION_FAILED;
    app->connection_lost_dirty = 1;
    app->help_visible = 0;
    va_start(arguments, format);
    vsnprintf(app->connection_lost_message,
              sizeof(app->connection_lost_message),
              format, arguments);
    va_end(arguments);
    request_full_redraw(app);
}

static void prepare_reconnect(struct app *app, time_t now)
{
    if (app->socket_fd >= 0)
        close(app->socket_fd);
    app->socket_fd = -1;
    app->authenticated = 0;
    app->login_complete = 0;
    app->login_failed = 0;
    app->reconnect_requested = 0;
    app->reconnect_login_deadline = 0;
    app->initial_state_requested = 0;
    app->version_request_deadline = 0;
    app->peer_refresh_pending = 0;
    app->peer_request_inflight = 0;
    app->peer_refresh_queued = 0;
    /* Partial frames belong to the dead socket and must not survive retry. */
    ami_parser_init(&app->parser);
    app->connection_state = CONNECTION_INTERRUPTED;
    app->next_reconnect = now + 3;
    mark_panel_dirty(app, PANEL_CHANNELS);
}

static void reconnect_attempt_failed(struct app *app, time_t now,
                                     const char *message)
{
    if (app->socket_fd >= 0)
        close(app->socket_fd);
    app->socket_fd = -1;
    app->authenticated = 0;
    app->login_complete = 0;
    app->login_failed = 0;
    if (app->reconnect_attempts >= 3) {
        mark_connection_lost(app,
                             "AMI reconnect failed after 3 attempts: %s",
                             message);
    } else {
        app->connection_state = CONNECTION_INTERRUPTED;
        app->next_reconnect = now + 3;
        mark_panel_dirty(app, PANEL_CHANNELS);
    }
}

/* UTF-8 aware text measurement, clipping, and horizontal scrolling. */

static void format_duration(time_t created_at, char *buffer, size_t size)
{
    long seconds = (long)(time(NULL) - created_at);

    if (seconds < 0)
        seconds = 0;
    snprintf(buffer, size, "%02ld:%02ld:%02ld", seconds / 3600,
             (seconds / 60) % 60, seconds % 60);
}

static void print_limited(const char *text, int width)
{
    const char *cursor = text[0] != '\0' ? text : "-";
    mbstate_t state;
    int columns = 0;

    memset(&state, 0, sizeof(state));
    while (*cursor != '\0' && columns < width) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, cursor, MB_CUR_MAX, &state);
        int character_width;

        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            fputc('?', stdout);
            cursor++;
            columns++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (bytes == 0)
            break;

        character_width = wcwidth(character);
        if (character_width < 0)
            character_width = 1;
        if (columns + character_width > width)
            break;
        fwrite(cursor, 1, bytes, stdout);
        cursor += bytes;
        columns += character_width;
    }
    while (columns < width) {
        putchar(' ');
        columns++;
    }
}

static size_t text_display_width(const char *text)
{
    mbstate_t state;
    size_t columns = 0;

    memset(&state, 0, sizeof(state));
    while (*text != '\0') {
        wchar_t character;
        size_t bytes = mbrtowc(&character, text, MB_CUR_MAX, &state);
        int character_width;

        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            text++;
            columns++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (bytes == 0)
            break;
        character_width = wcwidth(character);
        columns += (size_t)(character_width > 0 ? character_width : 1);
        text += bytes;
    }
    return columns;
}

static void print_scrolled(const char *text, int width, size_t *offset)
{
    const char *cursor = text;
    mbstate_t state;
    size_t total = text_display_width(text);
    size_t content_width;
    size_t max_offset;
    size_t skipped = 0;
    int printed = 0;
    int show_left;
    int show_right;

    if (width <= 0)
        return;
    if (total <= (size_t)width) {
        *offset = 0;
        print_limited(text, width);
        return;
    }
    max_offset = total > (size_t)(width - 1)
                     ? total - (size_t)(width - 1)
                     : 0;
    if (*offset > max_offset)
        *offset = max_offset;
    show_left = *offset > 0;
    content_width = (size_t)width - (show_left ? 1U : 0U);
    show_right = *offset + content_width < total;
    if (show_right && content_width > 0)
        content_width--;

    if (show_left)
        putchar('<');
    memset(&state, 0, sizeof(state));
    while (*cursor != '\0' && skipped < *offset) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, cursor, MB_CUR_MAX, &state);
        int character_width;

        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            cursor++;
            skipped++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (bytes == 0)
            break;
        character_width = wcwidth(character);
        if (character_width < 1)
            character_width = 1;
        cursor += bytes;
        skipped += (size_t)character_width;
    }
    memset(&state, 0, sizeof(state));
    while (*cursor != '\0' && printed < (int)content_width) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, cursor, MB_CUR_MAX, &state);
        int character_width;

        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            putchar('?');
            cursor++;
            printed++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (bytes == 0)
            break;
        character_width = wcwidth(character);
        if (character_width < 1)
            character_width = 1;
        if (printed + character_width > (int)content_width)
            break;
        fwrite(cursor, 1, bytes, stdout);
        cursor += bytes;
        printed += character_width;
    }
    while (printed++ < (int)content_width)
        putchar(' ');
    if (show_right)
        putchar('>');
}

static void append_text(char *buffer, size_t size, const char *text)
{
    size_t used = strlen(buffer);

    if (used + 1 < size)
        strncat(buffer, text, size - used - 1);
}

static void append_field(char *buffer, size_t size, const char *text,
                         int width)
{
    const char *cursor = text[0] != '\0' ? text : "-";
    mbstate_t state;
    int columns = 0;

    memset(&state, 0, sizeof(state));
    while (*cursor != '\0' && columns < width) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, cursor, MB_CUR_MAX, &state);
        int character_width;
        char part[MB_LEN_MAX + 1];

        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            append_text(buffer, size, "?");
            cursor++;
            columns++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        if (bytes == 0)
            break;
        character_width = wcwidth(character);
        if (character_width < 1)
            character_width = 1;
        if (columns + character_width > width)
            break;
        if (bytes > MB_LEN_MAX)
            break;
        memcpy(part, cursor, bytes);
        part[bytes] = '\0';
        append_text(buffer, size, part);
        cursor += bytes;
        columns += character_width;
    }
    while (columns++ < width)
        append_text(buffer, size, " ");
}

/* Low-level terminal drawing primitives shared by all panels. */

static void move_cursor(int row, int column)
{
    printf("\033[%d;%dH", row, column);
}

static void draw_box(int row, int column, int height, int width,
                     const char *title, int focused)
{
    int i;
    char decorated[256];

    if (height < 2 || width < 4)
        return;

    move_cursor(row, column);
    fputs("╔", stdout);
    for (i = 0; i < width - 2; i++)
        fputs("═", stdout);
    fputs("╗", stdout);
    for (i = 1; i < height - 1; i++) {
        move_cursor(row + i, column);
        fputs("║", stdout);
        move_cursor(row + i, column + width - 1);
        fputs("║", stdout);
    }
    move_cursor(row + height - 1, column);
    fputs("╚", stdout);
    for (i = 0; i < width - 2; i++)
        fputs("═", stdout);
    fputs("╝", stdout);

    if (title[0] != '\0' && width > 6) {
        snprintf(decorated, sizeof(decorated), " %s ", title);
        move_cursor(row, column + 2);
        if (focused)
            fputs("\033[1;7m", stdout);
        print_limited(decorated,
                      (int)strlen(decorated) < width - 4
                          ? (int)strlen(decorated)
                          : width - 4);
        if (focused)
            fputs("\033[0m", stdout);
    }
}

static void clear_box_interior(int row, int column, int height, int width)
{
    int i;
    int j;

    if (height < 3 || width < 3)
        return;
    for (i = 1; i < height - 1; i++) {
        move_cursor(row + i, column + 1);
        for (j = 0; j < width - 2; j++)
            putchar(' ');
    }
}

static void draw_table_separator(int row, int column, int width)
{
    int i;

    if (width < 3)
        return;
    move_cursor(row, column);
    fputs("╟", stdout);
    for (i = 0; i < width - 2; i++)
        fputs("─", stdout);
    fputs("╢", stdout);
}

static void draw_scrollbar(int row, int column, int height,
                           size_t total, size_t visible, size_t offset)
{
    int track_height = height - 2;
    int thumb_height;
    int thumb_start;
    int i;

    if (track_height <= 0 || visible == 0 || total <= visible)
        return;
    /* Scale both thumb size and position to the visible list fraction. */
    thumb_height = (int)((visible * (size_t)track_height) / total);
    if (thumb_height < 1)
        thumb_height = 1;
    thumb_start = (int)((offset * (size_t)(track_height - thumb_height)) /
                        (total - visible));
    for (i = 0; i < track_height; i++) {
        move_cursor(row + 1 + i, column);
        fputs(i >= thumb_start && i < thumb_start + thumb_height
                  ? "█"
                  : "░",
              stdout);
    }
}

static const struct astv_channel *selected_channel(struct app *app)
{
    size_t i;

    if (app->channels.count == 0) {
        app->channel_cursor = 0;
        app->channel_offset = 0;
        app->selected_uniqueid[0] = '\0';
        return NULL;
    }

    if (app->selected_uniqueid[0] != '\0') {
        for (i = 0; i < app->channels.count; i++) {
            if (strcmp(app->channels.channels[i].uniqueid,
                       app->selected_uniqueid) == 0) {
                app->channel_cursor = i;
                return &app->channels.channels[i];
            }
        }
    }

    if (app->channel_cursor >= app->channels.count)
        app->channel_cursor = app->channels.count - 1;
    copy_text(app->selected_uniqueid, sizeof(app->selected_uniqueid),
              app->channels.channels[app->channel_cursor].uniqueid);
    app->selected_event_cursor = EVENT_CURSOR_FOLLOW;
    return &app->channels.channels[app->channel_cursor];
}

/* Panel-specific table rendering. */

static int event_matches_channel(const struct event_entry *event,
                                 const struct astv_channel *channel)
{
    if (channel == NULL)
        return 0;
    return (event->uniqueid[0] != '\0' &&
            strcmp(event->uniqueid, channel->uniqueid) == 0) ||
           (event->dest_uniqueid[0] != '\0' &&
            strcmp(event->dest_uniqueid, channel->uniqueid) == 0) ||
           (event->channel[0] != '\0' &&
            strcmp(event->channel, channel->channel) == 0) ||
           (event->dest_channel[0] != '\0' &&
            strcmp(event->dest_channel, channel->channel) == 0);
}

static void draw_channels_panel(struct app *app, int row, int column,
                                int height, int width)
{
    const struct astv_channel *selected = selected_channel(app);
    int visible_rows = height - 4;
    int caller_width = 14;
    int connected_width = 14;
    int state_width = 8;
    int duration_width = 8;
    int channel_width = 32;
    int bridge_width = 12;
    int required_width;
    size_t i;
    char title[160];

    snprintf(title, sizeof(title),
             "CH:%zu  CL:%zu  %s%s%s  Tab/Shift+Tab: panel  h: help  q: quit",
             app->channels.count, channel_store_call_count(&app->channels),
             app->connection_state == CONNECTION_UP
                 ? "[+]"
                 : (app->connection_state == CONNECTION_RECONNECTING
                        ? "[?]"
                        : "[!]"),
             app->asterisk_version[0] != '\0' ? " AST:" : "",
             app->asterisk_version);
    draw_box(row, column, height, width, title,
             app->focus == PANEL_CHANNELS);
    if (height < 4 || width < 12)
        return;
    clear_box_interior(row, column, height, width);

    required_width = caller_width + connected_width + state_width +
                     duration_width + channel_width + bridge_width + 6;
    if (width - 2 < required_width) {
        caller_width = 10;
        connected_width = 10;
        state_width = 7;
        channel_width = width - 2 - caller_width - connected_width -
                        state_width - duration_width - bridge_width - 5;
        if (channel_width < 12) {
            caller_width = 8;
            connected_width = 8;
            state_width = 6;
            bridge_width = 8;
            channel_width = width - 2 - caller_width - connected_width -
                            state_width - duration_width - bridge_width - 5;
        }
        if (channel_width < 8)
            channel_width = 8;
    }

    move_cursor(row + 1, column + 1);
    print_limited("CALLER", caller_width);
    putchar(' ');
    print_limited("CONNECTED", connected_width);
    putchar(' ');
    print_limited("STATE", state_width);
    putchar(' ');
    print_limited("DURATION", duration_width);
    putchar(' ');
    print_limited("CHANNEL", channel_width);
    putchar(' ');
    print_limited("BRIDGE", bridge_width);
    draw_table_separator(row + 2, column, width);

    if (selected == NULL)
        return;
    if (app->channel_cursor < app->channel_offset)
        app->channel_offset = app->channel_cursor;
    if (app->channel_cursor >=
        app->channel_offset + (size_t)visible_rows)
        app->channel_offset =
            app->channel_cursor - (size_t)visible_rows + 1;

    for (i = 0; i < (size_t)visible_rows &&
                app->channel_offset + i < app->channels.count; i++) {
        size_t channel_index = app->channel_offset + i;
        const struct astv_channel *channel =
            &app->channels.channels[channel_index];
        char duration[32];
        char caller[192];
        char connected[192];
        char line[768] = "";

        format_duration(channel->created_at, duration, sizeof(duration));
        snprintf(caller, sizeof(caller), "%s%s%s",
                 channel->caller_num,
                 channel->caller_name[0] != '\0' ? " " : "",
                 channel->caller_name);
        snprintf(connected, sizeof(connected), "%s%s%s",
                 channel->connected_num,
                 channel->connected_name[0] != '\0' ? " " : "",
                 channel->connected_name);
        append_field(line, sizeof(line), caller, caller_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), connected, connected_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), channel->state, state_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), duration, duration_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), channel->channel, channel_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), channel->bridge_id, bridge_width);
        move_cursor(row + 3 + (int)i, column + 1);
        if (channel_index == app->channel_cursor) {
            fputs("\033[7m", stdout);
            print_scrolled(line, width - 3,
                           &app->horizontal_offset[PANEL_CHANNELS]);
            fputs("\033[0m", stdout);
        } else {
            print_limited(line, width - 3);
        }
    }
    draw_scrollbar(row + 2, column + width - 2, height - 2,
                   app->channels.count, (size_t)visible_rows,
                   app->channel_offset);
}

static int compare_peers(const void *left, const void *right)
{
    const struct peer_entry *first = left;
    const struct peer_entry *second = right;
    int comparison = strcasecmp(first->name, second->name);

    if (comparison == 0)
        comparison = strcmp(first->driver, second->driver);
    return comparison;
}

static char request_status_symbol(const char *status)
{
    if (strcmp(status, "accepted") == 0)
        return '+';
    if (strcmp(status, "error") == 0)
        return '-';
    return '?';
}

static void draw_peers_panel(struct app *app, int row, int column,
                             int height, int width)
{
    int visible_rows = height - 4;
    int type_width = 6;
    int name_width = 26;
    int status_width = 14;
    int address_width;
    int diagnostics_width;
    int title_width;
    size_t i;
    char title[64];
    char diagnostics[256];
    char pjsip_diagnostics[64];
    char header[256] = "";

    if (app->peers_dirty) {
        size_t selected = app->peer_active.count;

        if (app->peer_active.count > 1)
            qsort(app->peer_active.peers, app->peer_active.count,
                  sizeof(app->peer_active.peers[0]),
                  compare_peers);
        for (i = 0; i < app->peer_active.count; i++) {
            if (strcmp(app->peer_active.peers[i].driver,
                       app->selected_peer_driver) == 0 &&
                strcmp(app->peer_active.peers[i].name,
                       app->selected_peer_name) == 0) {
                selected = i;
                break;
            }
        }
        if (selected < app->peer_active.count)
            app->peer_cursor = selected;
        else if (app->peer_active.count > 0 &&
                 app->peer_cursor >= app->peer_active.count)
            app->peer_cursor = app->peer_active.count - 1;
        if (app->peer_active.count > 0) {
            copy_text(app->selected_peer_driver,
                      sizeof(app->selected_peer_driver),
                      app->peer_active.peers[app->peer_cursor].driver);
            copy_text(app->selected_peer_name,
                      sizeof(app->selected_peer_name),
                      app->peer_active.peers[app->peer_cursor].name);
        }
        app->peers_dirty = 0;
    }

    snprintf(title, sizeof(title), "PEERS: %zu",
             app->peer_active.count);
    {
        char endpoint_status =
            request_status_symbol(
                app->peer_active.pjsip_endpoint_status);
        char contact_status =
            request_status_symbol(
                app->peer_active.pjsip_contact_status);

        if (endpoint_status == '+') {
            char contacts[24];

            if (contact_status == '+')
                snprintf(contacts, sizeof(contacts), "%zu",
                         app->peer_active.pjsip_contact_count);
            else if (app->peer_active.pjsip_embedded_contact_count > 0)
                snprintf(contacts, sizeof(contacts), "%zu",
                         app->peer_active.pjsip_embedded_contact_count);
            else
                snprintf(contacts, sizeof(contacts), "%c",
                         contact_status);
            snprintf(pjsip_diagnostics, sizeof(pjsip_diagnostics),
                     "PJSIP:+ E:%zu C:%s",
                     app->peer_active.pjsip_endpoint_count,
                     contacts);
        } else {
            snprintf(pjsip_diagnostics, sizeof(pjsip_diagnostics),
                     "PJSIP:%c", endpoint_status);
        }
    }
    snprintf(diagnostics, sizeof(diagnostics),
             " SIP:%c/%zu IAX2:%c/%zu %s ",
             request_status_symbol(app->peer_active.sip_status),
             app->peer_active.sip_count,
             request_status_symbol(app->peer_active.iax_status),
             app->peer_active.iax_count,
             pjsip_diagnostics);
    draw_box(row, column, height, width, title,
             app->focus == PANEL_PEERS);
    if (height < 4 || width < 12)
        return;
    title_width = (int)strlen(title) + 2;
    diagnostics_width = width - title_width - 3;
    if (diagnostics_width > (int)strlen(diagnostics))
        diagnostics_width = (int)strlen(diagnostics);
    if (diagnostics_width > 0) {
        move_cursor(row, column + width - diagnostics_width - 1);
        print_limited(diagnostics, diagnostics_width);
    }
    clear_box_interior(row, column, height, width);

    address_width =
        width - 3 - type_width - name_width - status_width - 3;
    if (address_width < 8) {
        name_width = 18;
        status_width = 12;
        address_width =
            width - 3 - type_width - name_width - status_width - 3;
    }
    if (address_width < 4)
        address_width = 4;

    append_field(header, sizeof(header), "TYPE", type_width);
    append_text(header, sizeof(header), " ");
    append_field(header, sizeof(header), "NAME", name_width);
    append_text(header, sizeof(header), " ");
    append_field(header, sizeof(header), "STATUS", status_width);
    append_text(header, sizeof(header), " ");
    append_field(header, sizeof(header), "ADDRESS", address_width);
    move_cursor(row + 1, column + 1);
    print_limited(header, width - 3);
    draw_table_separator(row + 2, column, width);

    if (app->peer_active.count == 0 &&
        app->peer_active.error[0] != '\0') {
        move_cursor(row + 3, column + 2);
        print_limited(app->peer_active.error, width - 4);
        return;
    }
    if (app->peer_offset >= app->peer_active.count)
        app->peer_offset = 0;
    if (app->peer_active.count > 0 &&
        app->peer_cursor >= app->peer_active.count)
        app->peer_cursor = app->peer_active.count - 1;
    if (app->peer_cursor < app->peer_offset)
        app->peer_offset = app->peer_cursor;
    if (visible_rows > 0 &&
        app->peer_cursor >= app->peer_offset + (size_t)visible_rows)
        app->peer_offset =
            app->peer_cursor - (size_t)visible_rows + 1;
    for (i = 0; i < (size_t)visible_rows &&
                app->peer_offset + i < app->peer_active.count; i++) {
        size_t peer_index = app->peer_offset + i;
        const struct peer_entry *peer =
            &app->peer_active.peers[peer_index];
        char line[512] = "";

        append_field(line, sizeof(line), peer->driver, type_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), peer->name, name_width);
        append_text(line, sizeof(line), " ");
        append_field(line, sizeof(line), peer->status, status_width);
        append_text(line, sizeof(line), " ");
        append_text(line, sizeof(line),
                    peer->address[0] != '\0' ? peer->address : "-");
        move_cursor(row + 3 + (int)i, column + 1);
        if (peer_index == app->peer_cursor) {
            if (app->focus == PANEL_PEERS)
                fputs("\033[7m", stdout);
            print_scrolled(line, width - 3,
                           &app->horizontal_offset[PANEL_PEERS]);
            if (app->focus == PANEL_PEERS)
                fputs("\033[0m", stdout);
        } else {
            print_limited(line, width - 3);
        }
    }
    draw_scrollbar(row + 2, column + width - 2, height - 2,
                   app->peer_active.count, (size_t)visible_rows,
                   app->peer_offset);
}

static void draw_events_panel(struct app *app,
                              const struct astv_channel *selected,
                              int selected_events, int row, int column,
                              int height, int width, const char *title,
                              size_t *event_cursor,
                              enum panel_focus panel)
{
    size_t indexes[EVENT_LOG_SIZE];
    size_t count = 0;
    size_t offset;
    size_t visible;
    size_t cursor;
    size_t i;
    int visible_rows = height - 2;

    draw_box(row, column, height, width, title, app->focus == panel);
    if (visible_rows <= 0 || width < 4)
        return;
    clear_box_interior(row, column, height, width);

    for (i = 0; i < app->event_count; i++) {
        int matches = event_matches_channel(event_at(app, i), selected);

        if ((selected_events && matches) || !selected_events)
            indexes[count++] = i;
    }
    if (count == 0) {
        *event_cursor = 0;
        return;
    }
    if (*event_cursor == EVENT_CURSOR_FOLLOW) {
        cursor = count - 1;
    } else {
        if (*event_cursor >= count)
            *event_cursor = count - 1;
        cursor = *event_cursor;
    }
    visible = (size_t)visible_rows;
    offset = count > visible ? count - visible : 0;
    if (cursor < offset)
        offset = cursor;
    if (cursor >= offset + visible)
        offset = cursor - visible + 1;

    for (i = 0; i < visible && offset + i < count; i++) {
        size_t event_index = offset + i;
        const struct event_entry *event =
            event_at(app, indexes[event_index]);

        move_cursor(row + 1 + (int)i, column + 1);
        if (event_index == cursor) {
            if (app->focus == panel)
                fputs("\033[7m", stdout);
            print_scrolled(event->text, width - 3,
                           &app->horizontal_offset[panel]);
            if (app->focus == panel)
                fputs("\033[0m", stdout);
        } else {
            print_limited(event->text, width - 3);
        }
    }
    draw_scrollbar(row, column + width - 2, height,
                   count, visible, offset);
}

static const char *const help_lines[] = {
    "astv - Asterisk AMI terminal monitor",
    "Version: " ASTV_VERSION,
    "Author: Yuriy_Y",
    "",
    "KEYS",
    "  Tab         switch to the next panel",
    "  Shift+Tab   switch to the previous panel",
    "  Up/Down     move or scroll the focused panel",
    "  PgUp/PgDown move one page in the focused panel",
    "  Left/Right  scroll the selected row horizontally",
    "  Home/End    first or last item",
    "  h / Esc     open or close this help",
    "  q           quit astv (ignored while help is open)",
    "",
    "TOP PANEL HEADERS",
    "  CH:N         number of active Asterisk channels",
    "  CL:N         number of calls grouped by Linkedid",
    "  AST:V        detected Asterisk version",
    "  [+]/[!]/[?]  AMI up, interrupted, or reconnecting",
    "  PEERS: N     total SIP, IAX2, and PJSIP entries",
    "  SIP:s/N      SIP request status and peer count",
    "  IAX2:s/N     IAX2 request status and peer count",
    "  PJSIP:+ E:N C:N/-/?",
    "               endpoints work; contacts show count/status",
    "  PJSIP:-/?    endpoint request error or pending",
    "  s is + for success, - for error, ? while pending",
    "",
    "COMMAND-LINE OPTIONS",
    "  -H, --host HOST          AMI host (default: 127.0.0.1)",
    "  -p, --port PORT          AMI port (default: 5038)",
    "  -u, --user USER          AMI username (default: monitor)",
    "  -s, --secret-file FILE   read the AMI password from a file",
    "  -c, --config FILE        config file (default: /etc/astv/astv.conf)",
    "  -h, --help               command-line help",
    "",
    "ENVIRONMENT",
    "  ASTV_SECRET              overrides the password from the config",
    "",
    "EXAMPLE /etc/asterisk/manager.conf",
    "  [general]",
    "  enabled = yes",
    "  port = 5038",
    "  bindaddr = 127.0.0.1",
    "",
    "  [astv]",
    "  secret = replace-with-a-long-random-secret",
    "  read = system,call,reporting,command",
    "  write = system,call,reporting",
    "  permit = 127.0.0.1/255.255.255.255",
    "",
    "Reload the configuration:",
    "  asterisk -rx \"manager reload\"",
    "",
    "Do not expose plain AMI port 5038 to the public Internet.",
    "Use a VPN, SSH tunnel, or AMI TLS for remote access."
};

static void draw_help(struct app *app, int rows, int columns)
{
    size_t line_count = sizeof(help_lines) / sizeof(help_lines[0]);
    int width = columns - 6;
    int height = rows - 4;
    int row;
    int column;
    int visible_rows;
    size_t i;

    if (width > 94)
        width = 94;
    if (height > 32)
        height = 32;
    if (width < 60)
        width = columns - 2;
    if (height < 10)
        height = rows - 2;
    row = (rows - height) / 2 + 1;
    column = (columns - width) / 2 + 1;
    visible_rows = height - 2;

    if (app->help_offset + (size_t)visible_rows > line_count) {
        if (line_count > (size_t)visible_rows)
            app->help_offset = line_count - (size_t)visible_rows;
        else
            app->help_offset = 0;
    }

    draw_box(row, column, height, width,
             "HELP  Up/Down: scroll  h/Esc: close", 1);
    clear_box_interior(row, column, height, width);
    for (i = 0; i < (size_t)visible_rows; i++) {
        const char *line = "";

        if (app->help_offset + i < line_count)
            line = help_lines[app->help_offset + i];
        move_cursor(row + 1 + (int)i, column + 1);
        if (line[0] != '\0')
            print_limited(line, width - 3);
    }
    draw_scrollbar(row, column + width - 2, height, line_count,
                   (size_t)visible_rows, app->help_offset);
}

static void draw_connection_lost(struct app *app, int rows, int columns)
{
    int width = columns - 8;
    int height = 7;
    int row;
    int column;

    if (width > 76)
        width = 76;
    if (width < 40)
        width = columns - 2;
    if (height > rows - 2)
        height = rows - 2;
    row = (rows - height) / 2 + 1;
    column = (columns - width) / 2 + 1;

    draw_box(row, column, height, width,
             "AMI CONNECTION LOST", 1);
    clear_box_interior(row, column, height, width);
    move_cursor(row + 2, column + 2);
    print_limited(app->connection_lost_message, width - 4);
    move_cursor(row + 4, column + 2);
    print_limited("Press q to quit astv.", width - 4);
}

static void draw_screen(struct app *app)
{
    struct winsize window = {0};
    const struct astv_channel *selected;
    unsigned panels;
    int rows = 24;
    int columns = 100;
    int left_width;
    int right_width;
    int top_height;
    int bottom_height;
    int rendered = 0;
    char selected_title[192];

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        if (window.ws_row > 0)
            rows = window.ws_row;
        if (window.ws_col > 0)
            columns = window.ws_col;
    }
    if (rows != app->screen_rows || columns != app->screen_columns) {
        app->screen_rows = rows;
        app->screen_columns = columns;
        request_full_redraw(app);
    }
    if (app->full_redraw)
        printf("\033[H\033[2J");
    if (rows < 16 || columns < 72) {
        if (app->full_redraw) {
            printf("Terminal is too small. Minimum size: 72x16; current: %dx%d",
                   columns, rows);
            fflush(stdout);
        }
        app->dirty_panels = 0;
        app->full_redraw = 0;
        app->help_dirty = 0;
        return;
    }

    left_width = columns * 3 / 5;
    right_width = columns - left_width + 1;
    top_height = rows * 3 / 4;
    bottom_height = rows - top_height + 1;

    selected = selected_channel(app);
    if (selected != NULL)
        snprintf(selected_title, sizeof(selected_title),
                 "SELECTED EVENTS: %s", selected->channel);
    else
        copy_text(selected_title, sizeof(selected_title),
                  "SELECTED EVENTS: no channel");

    /* Redraw only dirty panels unless resize/modal state requires all four. */
    panels = app->full_redraw ? ALL_PANEL_BITS : app->dirty_panels;
    if ((!app->help_visible && !app->connection_lost) ||
        app->full_redraw) {
        rendered = panels != 0;
        if (panels & PANEL_BIT(PANEL_CHANNELS))
            draw_channels_panel(app, 1, 1, top_height, left_width);
        if (panels & PANEL_BIT(PANEL_PEERS))
            draw_peers_panel(app, 1, left_width, top_height, right_width);
        if (panels & PANEL_BIT(PANEL_ALL_EVENTS))
            draw_events_panel(app, selected, 0, top_height, 1,
                              bottom_height, left_width, "ALL AMI EVENTS",
                              &app->all_event_cursor, PANEL_ALL_EVENTS);
        if (panels & PANEL_BIT(PANEL_SELECTED_EVENTS))
            draw_events_panel(app, selected, 1, top_height, left_width,
                              bottom_height, right_width, selected_title,
                              &app->selected_event_cursor,
                              PANEL_SELECTED_EVENTS);
        if (panels != 0) {
            move_cursor(1, left_width);
            fputs("╦", stdout);
            if (panels & (PANEL_BIT(PANEL_CHANNELS) |
                          PANEL_BIT(PANEL_PEERS))) {
                move_cursor(3, left_width);
                fputs("╫", stdout);
            }
            move_cursor(top_height, 1);
            fputs("╠", stdout);
            move_cursor(top_height, left_width);
            fputs("╬", stdout);
            move_cursor(top_height, columns);
            fputs("╣", stdout);
            move_cursor(rows, left_width);
            fputs("╩", stdout);
        }
        app->dirty_panels &= ~panels;
    }
    if (app->help_visible && (app->full_redraw || app->help_dirty)) {
        draw_help(app, rows, columns);
        rendered = 1;
    }
    if (app->connection_lost &&
        (app->full_redraw || app->connection_lost_dirty)) {
        draw_connection_lost(app, rows, columns);
        rendered = 1;
    }
    if (rendered) {
        move_cursor(rows, columns);
        fflush(stdout);
    }
    app->full_redraw = 0;
    app->help_dirty = 0;
    app->connection_lost_dirty = 0;
}

/* Alternate-screen terminal lifecycle and focused-panel navigation. */

static void configure_terminal(struct app *app)
{
    struct termios terminal;

    if (!isatty(STDIN_FILENO) ||
        tcgetattr(STDIN_FILENO, &app->saved_termios) < 0)
        return;
    terminal = app->saved_termios;
    terminal.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    terminal.c_cc[VMIN] = 0;
    terminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal) == 0)
        app->terminal_configured = 1;
}

static void restore_terminal(struct app *app)
{
    if (app->terminal_configured)
        tcsetattr(STDIN_FILENO, TCSANOW, &app->saved_termios);
    printf("\033[0m\033[?25h\033[?1049l");
    fflush(stdout);
}

static void select_channel_at_cursor(struct app *app)
{
    if (app->channels.count == 0) {
        app->selected_uniqueid[0] = '\0';
        mark_panels_dirty(app, PANEL_BIT(PANEL_CHANNELS) |
                               PANEL_BIT(PANEL_SELECTED_EVENTS));
        return;
    }
    if (app->channel_cursor >= app->channels.count)
        app->channel_cursor = app->channels.count - 1;
    copy_text(app->selected_uniqueid, sizeof(app->selected_uniqueid),
              app->channels.channels[app->channel_cursor].uniqueid);
    app->horizontal_offset[PANEL_CHANNELS] = 0;
    app->horizontal_offset[PANEL_SELECTED_EVENTS] = 0;
    app->selected_event_cursor = EVENT_CURSOR_FOLLOW;
    mark_panels_dirty(app, PANEL_BIT(PANEL_CHANNELS) |
                           PANEL_BIT(PANEL_SELECTED_EVENTS));
}

static size_t selected_event_count(struct app *app)
{
    const struct astv_channel *channel = selected_channel(app);
    size_t count = 0;
    size_t i;

    for (i = 0; i < app->event_count; i++) {
        if (event_matches_channel(event_at(app, i), channel))
            count++;
    }
    return count;
}

static size_t focused_item_count(struct app *app)
{
    switch (app->focus) {
    case PANEL_CHANNELS:
        return app->channels.count;
    case PANEL_PEERS:
        return app->peer_active.count;
    case PANEL_ALL_EVENTS:
        return app->event_count;
    case PANEL_SELECTED_EVENTS:
        return selected_event_count(app);
    default:
        return 0;
    }
}

static size_t *focused_cursor(struct app *app)
{
    switch (app->focus) {
    case PANEL_CHANNELS:
        return &app->channel_cursor;
    case PANEL_PEERS:
        return &app->peer_cursor;
    case PANEL_ALL_EVENTS:
        return &app->all_event_cursor;
    case PANEL_SELECTED_EVENTS:
        return &app->selected_event_cursor;
    default:
        return &app->channel_cursor;
    }
}

static void sync_focused_selection(struct app *app)
{
    /* Horizontal position resets when selection changes to a different row. */
    if (app->focus == PANEL_CHANNELS) {
        select_channel_at_cursor(app);
    } else {
        if (app->focus == PANEL_PEERS) {
            copy_text(app->selected_peer_driver,
                      sizeof(app->selected_peer_driver),
                      app->peer_active.peers[app->peer_cursor].driver);
            copy_text(app->selected_peer_name,
                      sizeof(app->selected_peer_name),
                      app->peer_active.peers[app->peer_cursor].name);
        }
        app->horizontal_offset[app->focus] = 0;
    }
    mark_panel_dirty(app, app->focus);
}

static void move_focused_cursor(struct app *app, int direction)
{
    size_t count = focused_item_count(app);
    size_t *cursor = focused_cursor(app);
    size_t current;

    if (count == 0)
        return;
    current = *cursor == EVENT_CURSOR_FOLLOW ? count - 1 : *cursor;
    if (direction < 0 && current > 0)
        current--;
    else if (direction > 0 && current + 1 < count)
        current++;

    if ((app->focus == PANEL_ALL_EVENTS ||
         app->focus == PANEL_SELECTED_EVENTS) &&
        current + 1 == count)
        *cursor = EVENT_CURSOR_FOLLOW;
    else
        *cursor = current;

    sync_focused_selection(app);
}

static void move_focused_edge(struct app *app, int to_end)
{
    size_t count = focused_item_count(app);
    size_t *cursor = focused_cursor(app);

    if (count == 0)
        return;
    if (to_end &&
        (app->focus == PANEL_ALL_EVENTS ||
         app->focus == PANEL_SELECTED_EVENTS))
        *cursor = EVENT_CURSOR_FOLLOW;
    else
        *cursor = to_end ? count - 1 : 0;
    sync_focused_selection(app);
}

static size_t focused_page_size(const struct app *app)
{
    int rows = app->screen_rows > 0 ? app->screen_rows : 24;
    int top_height = rows * 3 / 4;
    int bottom_height = rows - top_height + 1;
    int visible_rows;

    if (app->focus == PANEL_CHANNELS || app->focus == PANEL_PEERS)
        visible_rows = top_height - 4;
    else
        visible_rows = bottom_height - 2;
    return visible_rows > 0 ? (size_t)visible_rows : 1U;
}

static void move_focused_page(struct app *app, int direction)
{
    size_t count = focused_item_count(app);
    size_t *cursor = focused_cursor(app);
    size_t current;
    size_t step;

    if (count == 0)
        return;
    current = *cursor == EVENT_CURSOR_FOLLOW ? count - 1 : *cursor;
    step = focused_page_size(app);
    if (direction < 0)
        current = current > step ? current - step : 0;
    else if (step < count - current)
        current += step;
    else
        current = count - 1;

    if ((app->focus == PANEL_ALL_EVENTS ||
         app->focus == PANEL_SELECTED_EVENTS) &&
        current + 1 == count)
        *cursor = EVENT_CURSOR_FOLLOW;
    else
        *cursor = current;
    sync_focused_selection(app);
}

static void move_focused_horizontal(struct app *app, int direction)
{
    size_t *offset = &app->horizontal_offset[app->focus];

    if (direction < 0) {
        if (*offset > 4)
            *offset -= 4;
        else
            *offset = 0;
    } else {
        *offset += 4;
    }
    mark_panel_dirty(app, app->focus);
}

static size_t help_line_count(void)
{
    return sizeof(help_lines) / sizeof(help_lines[0]);
}

static long long monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (long long)now.tv_sec * 1000LL +
           (long long)now.tv_nsec / 1000000LL;
}

static void handle_escape_timeout(struct app *app)
{
    int state;

    /* A lone ESC closes help; multi-byte escape sequences continue parsing. */
    if (app->key_state == 0 ||
        monotonic_milliseconds() < app->escape_deadline_ms)
        return;

    state = app->key_state;
    app->key_state = 0;
    if (state == 1 && app->help_visible) {
        app->help_visible = 0;
        request_full_redraw(app);
    }
}

static void move_help(struct app *app, int direction, size_t step)
{
    size_t line_count = help_line_count();

    if (direction < 0) {
        app->help_offset =
            app->help_offset > step ? app->help_offset - step : 0;
    } else if (app->help_offset < line_count) {
        if (step < line_count - app->help_offset)
            app->help_offset += step;
        else
            app->help_offset = line_count;
    }
    app->help_dirty = 1;
}

static void change_focus(struct app *app, enum panel_focus focus)
{
    enum panel_focus previous = app->focus;

    if (previous == focus)
        return;
    app->focus = focus;
    mark_panels_dirty(app, PANEL_BIT(previous) | PANEL_BIT(focus));
}

static void handle_keyboard(struct app *app)
{
    char keys[16];
    ssize_t length = read(STDIN_FILENO, keys, sizeof(keys));
    ssize_t i;

    if (length <= 0)
        return;

    /* Decode terminal escape sequences incrementally across read calls. */
    for (i = 0; i < length; i++) {
        char key = keys[i];

        if (app->connection_lost) {
            app->key_state = 0;
            if (key == 'q' || key == 'Q')
                running = 0;
            continue;
        }
        if (app->key_state == 0) {
            if (key == '\033') {
                app->key_state = 1;
                app->escape_deadline_ms =
                    monotonic_milliseconds() + 100;
            } else if (app->help_visible &&
                       (key == 'h' || key == 'H')) {
                app->help_visible = 0;
                request_full_redraw(app);
            } else if (!app->help_visible &&
                       (key == 'h' || key == 'H')) {
                app->help_visible = 1;
                app->help_offset = 0;
                app->help_dirty = 1;
            } else if (app->help_visible) {
                continue;
            } else if (key == 'q' || key == 'Q') {
                running = 0;
            } else if (key == '\t') {
                change_focus(app, (enum panel_focus)(
                    (app->focus + 1) % PANEL_COUNT));
            }
        } else if (app->key_state == 1) {
            app->key_state = key == '[' ? 2 : 0;
            app->escape_deadline_ms = monotonic_milliseconds() + 100;
        } else if (app->key_state == 2) {
            if (key == '5' || key == '6') {
                app->key_parameter = key;
                app->key_state = 3;
                app->escape_deadline_ms =
                    monotonic_milliseconds() + 100;
                continue;
            }
            if (app->help_visible && key == 'A') {
                move_help(app, -1, 1);
            } else if (app->help_visible && key == 'B') {
                move_help(app, 1, 1);
            } else if (app->help_visible && key == 'H') {
                app->help_offset = 0;
                app->help_dirty = 1;
            } else if (app->help_visible && key == 'F') {
                app->help_offset = help_line_count();
                app->help_dirty = 1;
            } else if (app->help_visible) {
                continue;
            } else if (key == 'Z') {
                change_focus(app, (enum panel_focus)(
                    (app->focus + PANEL_COUNT - 1) % PANEL_COUNT));
            } else if (key == 'A') {
                move_focused_cursor(app, -1);
            } else if (key == 'B') {
                move_focused_cursor(app, 1);
            } else if (key == 'C') {
                move_focused_horizontal(app, 1);
            } else if (key == 'D') {
                move_focused_horizontal(app, -1);
            } else if (key == 'H') {
                move_focused_edge(app, 0);
            } else if (key == 'F') {
                move_focused_edge(app, 1);
            }
            app->key_state = 0;
        } else {
            if (key == '~') {
                int direction = app->key_parameter == '5' ? -1 : 1;

                if (!app->help_visible)
                    move_focused_page(app, direction);
            }
            app->key_state = 0;
        }
    }
}

/* Command-line parsing and secure interactive first-run configuration. */

static int read_secret_file(const char *path, char *secret, size_t size)
{
    FILE *file = fopen(path, "r");
    size_t length;

    if (file == NULL)
        return -1;
    if (fgets(secret, (int)size, file) == NULL) {
        fclose(file);
        return -1;
    }
    fclose(file);
    length = strlen(secret);
    while (length > 0 &&
           (secret[length - 1] == '\n' || secret[length - 1] == '\r'))
        secret[--length] = '\0';
    return length > 0 ? 0 : -1;
}

static int prompt_value(const char *label, const char *default_value,
                        char *destination, size_t size)
{
    char input[512];
    size_t length;

    printf("%s [%s]: ", label, default_value);
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin) == NULL)
        return -1;
    length = strlen(input);
    while (length > 0 &&
           (input[length - 1] == '\n' || input[length - 1] == '\r'))
        input[--length] = '\0';
    copy_text(destination, size, length > 0 ? input : default_value);
    return 0;
}

static int prompt_secret(char *destination, size_t size)
{
    struct termios saved;
    struct termios hidden;
    int hide_input = 0;
    char input[512];
    size_t length;

    printf("AMI password: ");
    fflush(stdout);
    /* Disable terminal echo only while the AMI secret is entered. */
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        hidden = saved;
        hidden.c_lflag &= (tcflag_t)~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) == 0)
            hide_input = 1;
    }
    if (fgets(input, sizeof(input), stdin) == NULL) {
        if (hide_input)
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        putchar('\n');
        return -1;
    }
    if (hide_input)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    putchar('\n');

    length = strlen(input);
    while (length > 0 &&
           (input[length - 1] == '\n' || input[length - 1] == '\r'))
        input[--length] = '\0';
    if (length == 0)
        return -1;
    copy_text(destination, size, input);
    memset(input, 0, sizeof(input));
    return 0;
}

static int first_run_setup(const char *path, struct astv_config *config)
{
    char error[256];

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "Configuration file %s does not exist and interactive "
                "input is unavailable.\n",
                path);
        return -1;
    }

    printf("astv first-run configuration\n");
    printf("Configuration will be saved to %s\n\n", path);
    if (prompt_value("AMI address", ASTV_DEFAULT_HOST, config->host,
                     sizeof(config->host)) < 0 ||
        prompt_value("AMI port", ASTV_DEFAULT_PORT, config->port,
                     sizeof(config->port)) < 0 ||
        prompt_value("AMI username", ASTV_DEFAULT_USER, config->username,
                     sizeof(config->username)) < 0 ||
        prompt_secret(config->secret, sizeof(config->secret)) < 0) {
        fprintf(stderr, "Configuration input was cancelled or incomplete.\n");
        return -1;
    }

    if (astv_config_save(path, config, error, sizeof(error)) < 0) {
        fprintf(stderr, "%s\n", error);
        if (strncmp(path, "/etc/", 5) == 0)
            fprintf(stderr,
                    "Run the first setup with sudo so astv can create %s.\n",
                    path);
        return -1;
    }
    printf("Configuration saved.\n\n");
    return 0;
}

static void usage(FILE *stream, const char *program, int full)
{
    size_t i;

    if (!full) {
        fprintf(stream,
                "Usage: %s [options]\n"
                "Try '%s --help' for more information.\n",
                program, program);
        return;
    }

    for (i = 0; i < help_line_count(); i++)
        fprintf(stream, "%s\n", help_lines[i]);
}

static int help_requested(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--help") == 0)
            return 1;
    }
    return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
    static const struct option long_options[] = {
        {"host", required_argument, NULL, 'H'},
        {"port", required_argument, NULL, 'p'},
        {"user", required_argument, NULL, 'u'},
        {"secret-file", required_argument, NULL, 's'},
        {"config", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    const char *environment_secret;
    const char *host_override = NULL;
    const char *port_override = NULL;
    const char *user_override = NULL;
    const char *secret_file = NULL;
    char error[256];
    int load_result;
    int option;

    astv_config_defaults(&options->connection);
    options->config_path = ASTV_DEFAULT_CONFIG;

    while ((option = getopt_long(argc, argv, "H:p:u:s:c:h", long_options,
                                 NULL)) != -1) {
        switch (option) {
        case 'H':
            host_override = optarg;
            break;
        case 'p':
            port_override = optarg;
            break;
        case 'u':
            user_override = optarg;
            break;
        case 's':
            secret_file = optarg;
            break;
        case 'c':
            options->config_path = optarg;
            break;
        case 'h':
            usage(stdout, argv[0], 1);
            exit(0);
        default:
            usage(stderr, argv[0], 0);
            return -1;
        }
    }

    load_result = astv_config_load(options->config_path,
                                   &options->connection, error,
                                   sizeof(error));
    if (load_result == 1) {
        if (first_run_setup(options->config_path,
                            &options->connection) < 0)
            return -1;
    } else if (load_result < 0) {
        fprintf(stderr, "Invalid configuration: %s\n", error);
        return -1;
    }

    if (host_override != NULL)
        copy_text(options->connection.host,
                  sizeof(options->connection.host), host_override);
    if (port_override != NULL)
        copy_text(options->connection.port,
                  sizeof(options->connection.port), port_override);
    if (user_override != NULL)
        copy_text(options->connection.username,
                  sizeof(options->connection.username), user_override);

    environment_secret = getenv("ASTV_SECRET");
    if (environment_secret != NULL)
        copy_text(options->connection.secret,
                  sizeof(options->connection.secret), environment_secret);
    if (secret_file != NULL &&
        read_secret_file(secret_file, options->connection.secret,
                         sizeof(options->connection.secret)) < 0) {
        fprintf(stderr, "Cannot read secret file: %s\n", secret_file);
        return -1;
    }
    if (astv_config_validate(&options->connection, error,
                             sizeof(error)) < 0) {
        fprintf(stderr, "Invalid configuration: %s\n", error);
        return -1;
    }
    return 0;
}

/* Process lifecycle and the poll-based AMI/TUI event loop. */

int main(int argc, char **argv)
{
    static struct app app;
    char startup_error[256];
    time_t last_draw = 0;

    if (help_requested(argc, argv)) {
        usage(stdout, argv[0], 1);
        return 0;
    }

    /* Root is required because the default configuration lives in /etc. */
    if (geteuid() != 0) {
        fprintf(stderr,
                "Permission error: astv must be run as root. "
                "Use: sudo %s\n",
                argv[0]);
        return 1;
    }

    (void)setlocale(LC_CTYPE, "");
    memset(&app, 0, sizeof(app));
    app.all_event_cursor = EVENT_CURSOR_FOLLOW;
    app.selected_event_cursor = EVENT_CURSOR_FOLLOW;
    app.socket_fd = -1;
    app.connection_state = CONNECTION_UP;
    app.full_redraw = 1;
    if (parse_options(argc, argv, &app.options) < 0)
        return 2;

    ami_parser_init(&app.parser);
    channel_store_init(&app.channels);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    if (startup_connect(&app, startup_error, sizeof(startup_error)) < 0) {
        fprintf(stderr, "Connection error: %s\n", startup_error);
        memset(app.options.connection.secret, 0,
               sizeof(app.options.connection.secret));
        return 1;
    }

    configure_terminal(&app);
    printf("\033[?1049h\033[2J\033[H\033[?25l");

    while (running) {
        struct pollfd descriptors[2];
        nfds_t descriptor_count = 1;
        int poll_result;
        time_t now = time(NULL);

        /* Start DNS/TCP reconnect work without blocking screen updates. */
        if (app.socket_fd < 0 && !app.connection_lost &&
            !app.reconnect_job.active &&
            now >= app.next_reconnect) {
            app.connection_state = CONNECTION_RECONNECTING;
            app.reconnect_attempts++;
            mark_panel_dirty(&app, PANEL_CHANNELS);
            if (astv_connect_job_start(
                    &app.reconnect_job,
                    app.options.connection.host,
                    app.options.connection.port,
                    STARTUP_TIMEOUT_MS) < 0) {
                reconnect_attempt_failed(
                    &app, now, app.reconnect_job.error);
            } else {
                app.reconnect_connect_deadline =
                    now + (STARTUP_TIMEOUT_MS + 999) / 1000;
            }
        }
        if (astv_connect_job_finished(&app.reconnect_job)) {
            struct astv_connect_job *job = &app.reconnect_job;

            astv_connect_job_finish(job);
            app.reconnect_connect_deadline = 0;
            app.socket_fd = job->fd;
            if (app.socket_fd >= 0) {
                ami_parser_init(&app.parser);
                app.login_complete = 0;
                app.login_failed = 0;
                app.login_error[0] = '\0';
                if (send_login(&app) < 0) {
                    reconnect_attempt_failed(
                        &app, now, "cannot send AMI Login");
                } else {
                    app.reconnect_login_deadline = now + 5;
                }
            } else {
                reconnect_attempt_failed(&app, now, job->error);
            }
        }
        if (app.reconnect_job.active &&
            app.reconnect_connect_deadline != 0 &&
            now >= app.reconnect_connect_deadline) {
            astv_connect_job_cancel(&app.reconnect_job);
            app.reconnect_connect_deadline = 0;
            reconnect_attempt_failed(
                &app, now, "DNS/TCP reconnect timed out");
        }

        if (app.socket_fd >= 0 && !app.authenticated &&
            app.login_failed) {
            reconnect_attempt_failed(
                &app, now,
                app.login_error[0] != '\0'
                    ? app.login_error
                    : "AMI authentication failed");
        }
        if (app.socket_fd >= 0 && !app.authenticated &&
            app.connection_state == CONNECTION_RECONNECTING &&
            app.reconnect_login_deadline != 0 &&
            now >= app.reconnect_login_deadline) {
            reconnect_attempt_failed(
                &app, now, "timeout waiting for AMI Login response");
        }

        if (app.socket_fd >= 0 && app.authenticated &&
            !app.initial_state_requested &&
            app.version_request_deadline != 0 &&
            now >= app.version_request_deadline) {
            add_event(&app,
                      "Asterisk version response timeout; using compatibility probes");
            if (request_initial_state(&app) < 0) {
                prepare_reconnect(&app, now);
            }
        }

        /* Deferred refreshes debounce multiple events from one call change. */
        if (app.socket_fd >= 0 && app.authenticated &&
            app.peer_refresh_pending && now >= app.peer_refresh_at) {
            app.peer_refresh_pending = 0;
            if (request_peers(&app) < 0) {
                prepare_reconnect(&app, now);
            }
        }
        if (app.peer_request_inflight &&
            now >= app.peer_request_deadline)
            timeout_peer_snapshot(&app);

        /* One poll drives both terminal input and AMI traffic. */
        descriptors[0].fd = STDIN_FILENO;
        descriptors[0].events = POLLIN;
        if (app.socket_fd >= 0) {
            descriptors[1].fd = app.socket_fd;
            descriptors[1].events = POLLIN;
            descriptor_count = 2;
        }

        poll_result = poll(descriptors, descriptor_count, 250);
        if (poll_result < 0 && errno != EINTR) {
            add_event(&app, "poll failed: %s", strerror(errno));
            break;
        }

        if (descriptors[0].revents & POLLIN) {
            handle_keyboard(&app);
        }
        handle_escape_timeout(&app);

        if (descriptor_count == 2 &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buffer[8192];
            ssize_t received = recv(app.socket_fd, buffer, sizeof(buffer), 0);

            if (received > 0) {
                if (ami_parser_feed(&app.parser, buffer, (size_t)received,
                                    handle_message, &app) < 0) {
                    add_event(&app, "AMI message exceeded parser buffer");
                    if (app.connection_state ==
                        CONNECTION_RECONNECTING)
                        reconnect_attempt_failed(
                            &app, time(NULL),
                            "AMI parser error during reconnect");
                    else
                        prepare_reconnect(&app, time(NULL));
                } else if (app.reconnect_requested) {
                    app.reconnect_requested = 0;
                    prepare_reconnect(&app, time(NULL));
                }
            } else {
                if (app.connection_state == CONNECTION_RECONNECTING)
                    reconnect_attempt_failed(
                        &app, time(NULL),
                        "AMI closed the connection during reconnect");
                else
                    prepare_reconnect(&app, time(NULL));
            }
        }

        now = time(NULL);
        if (!app.connection_lost && now != last_draw &&
            app.channels.count > 0)
            mark_panel_dirty(&app, PANEL_CHANNELS);
        /* Durations and connection indicators still refresh once per second. */
        if (app.dirty_panels != 0 || app.full_redraw ||
            app.help_dirty || app.connection_lost_dirty ||
            now != last_draw) {
            draw_screen(&app);
            last_draw = now;
        }
    }

    if (app.reconnect_job.active)
        astv_connect_job_cancel(&app.reconnect_job);
    if (app.socket_fd >= 0)
        close(app.socket_fd);
    memset(app.options.connection.secret, 0,
           sizeof(app.options.connection.secret));
    restore_terminal(&app);
    return 0;
}
