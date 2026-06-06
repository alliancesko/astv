#define _POSIX_C_SOURCE 200809L

#include "ami.h"
#include "asterisk_compat.h"
#include "channel_store.h"
#include "config.h"
#include "connection.h"

#include <assert.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Minimal regression suite for protocol and persistence modules. */

struct test_context {
    struct channel_store store;
    int messages;
    int truncated_messages;
};

static void collect(const struct ami_message *message, void *data)
{
    struct test_context *context = data;

    context->messages++;
    if (message->truncated)
        context->truncated_messages++;
    channel_store_apply(&context->store, message);
}

int main(void)
{
    struct ami_parser parser;
    struct test_context context;
    const char *part1 =
        "Event: Newchannel\r\nUniqueid: 1.2\r\nLinkedid: 1.2\r\n"
        "Channel: PJSIP/1001-0001\r\nCallerIDNum: 1001\r\n";
    const char *part2 =
        "ChannelStateDesc: Ring\r\n\r\n"
        "Event: Newstate\r\nUniqueid: 1.2\r\n"
        "ChannelStateDesc: Up\r\n\r\n";
    const char *hangup =
        "Event: Hangup\r\nUniqueid: 1.2\r\n\r\n";
    const char *status_event =
        "Event: Status\r\nUniqueid: 1.2\r\n"
        "BridgeID: bridge-22\r\nSeconds: 12\r\n\r\n";
    struct astv_asterisk_version version;
    struct astv_connect_job connect_job;
    struct timespec pause = {0, 10 * 1000 * 1000};
    struct astv_config saved;
    struct astv_config loaded;
    struct stat status;
    char config_path[128];
    char symlink_path[128];
    char symlink_target[128];
    char long_config_path[128];
    char many_fields[8192];
    char error[256];
    size_t used = 0;
    int field;

    /* Streaming AMI parsing and channel lifecycle. */
    memset(&context, 0, sizeof(context));
    channel_store_init(&context.store);
    ami_parser_init(&parser);

    assert(ami_parser_feed(&parser, part1, strlen(part1), collect,
                           &context) == 0);
    assert(context.messages == 0);
    assert(ami_parser_feed(&parser, part2, strlen(part2), collect,
                           &context) == 0);
    assert(context.messages == 2);
    assert(context.store.count == 1);
    assert(strcmp(context.store.channels[0].uniqueid, "1.2") == 0);
    assert(strcmp(context.store.channels[0].state, "Up") == 0);
    assert(channel_store_call_count(&context.store) == 1);
    assert(ami_parser_feed(&parser, status_event, strlen(status_event),
                           collect, &context) == 0);
    assert(strcmp(context.store.channels[0].bridge_id,
                  "bridge-22") == 0);

    /* Version compatibility thresholds and PJSIP contact parsing. */
    assert(astv_asterisk_version_parse("22.9.0",
                                      &version) == 0);
    assert(version.major == 22 && version.minor == 9 &&
           version.patch == 0);
    assert(astv_asterisk_version_parse("GIT-20-branch-20.11.1",
                                      &version) == 0);
    assert(version.major == 20);
    assert(astv_asterisk_supports_pjsip_contacts(&version));
    assert(astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("Asterisk 18.26.2",
                                      &version) == 0);
    assert(version.major == 18 && version.minor == 26);
    assert(astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("16.30.1-cert2",
                                      &version) == 0);
    assert(version.major == 16 && version.minor == 30);
    assert(!astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("20.11.1-custom",
                                      &version) == 0);
    assert(version.major == 20 && version.minor == 11);
    assert(astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("20.10.0", &version) == 0);
    assert(!astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("22.0.0", &version) == 0);
    assert(!astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("22.9.0", &version) == 0);
    assert(astv_asterisk_supports_status_snapshot(&version));
    assert(astv_asterisk_version_parse("unknown", &version) < 0);
    assert(astv_pjsip_contact_count("") == 0);
    assert(astv_pjsip_contact_count("<none>") == 0);
    assert(astv_pjsip_contact_count(
               "aor_trunk/sip:host@example.com:5060") == 1);
    assert(astv_pjsip_contact_count(
               "sip:one@example.com,sips:two@example.com") == 2);

    /* Asynchronous connection jobs publish a failed attempt cleanly. */
    memset(&connect_job, 0, sizeof(connect_job));
    assert(astv_connect_job_start(&connect_job, "127.0.0.1", "1",
                                  200) == 0);
    while (!astv_connect_job_finished(&connect_job))
        nanosleep(&pause, NULL);
    astv_connect_job_finish(&connect_job);
    assert(connect_job.fd < 0);

    assert(ami_parser_feed(&parser, hangup, strlen(hangup), collect,
                           &context) == 0);
    assert(context.store.count == 0);

    /* Oversized field sets are reported without corrupting parser state. */
    for (field = 0; field < AMI_MAX_FIELDS + 2; field++) {
        int written = snprintf(many_fields + used,
                               sizeof(many_fields) - used,
                               "Field%d: value\r\n", field);

        assert(written > 0);
        used += (size_t)written;
        assert(used + 2 < sizeof(many_fields));
    }
    memcpy(many_fields + used, "\r\n", 2);
    used += 2;
    assert(ami_parser_feed(&parser, many_fields, used, collect,
                           &context) == 0);
    assert(context.truncated_messages == 1);

    /* Configuration save/load is private, atomic, and symlink-resistant. */
    snprintf(config_path, sizeof(config_path), "/tmp/astv-test-%ld.conf",
             (long)getpid());
    astv_config_defaults(&loaded);
    assert(astv_config_load(config_path, &loaded, error, sizeof(error)) == 1);

    astv_config_defaults(&saved);
    strcpy(saved.host, "192.0.2.10");
    strcpy(saved.port, "5040");
    strcpy(saved.username, "test-user");
    strcpy(saved.secret, "test-secret");
    assert(astv_config_save(config_path, &saved, error, sizeof(error)) == 0);
    assert(stat(config_path, &status) == 0);
    assert((status.st_mode & 0777) == 0600);

    astv_config_defaults(&loaded);
    assert(astv_config_load(config_path, &loaded, error, sizeof(error)) == 0);
    assert(strcmp(loaded.host, saved.host) == 0);
    assert(strcmp(loaded.port, saved.port) == 0);
    assert(strcmp(loaded.username, saved.username) == 0);
    assert(strcmp(loaded.secret, saved.secret) == 0);

    strcpy(loaded.port, "70000");
    assert(astv_config_validate(&loaded, error, sizeof(error)) < 0);

    snprintf(symlink_path, sizeof(symlink_path),
             "/tmp/astv-test-%ld-link.conf", (long)getpid());
    snprintf(symlink_target, sizeof(symlink_target),
             "/tmp/astv-test-%ld-target.conf", (long)getpid());
    unlink(symlink_path);
    unlink(symlink_target);
    assert(symlink(symlink_target, symlink_path) == 0);
    assert(astv_config_save(symlink_path, &saved, error,
                            sizeof(error)) < 0);
    assert(lstat(symlink_path, &status) == 0);
    assert(S_ISLNK(status.st_mode));
    assert(access(symlink_target, F_OK) < 0);

    /* Overlong configuration lines must fail instead of being split. */
    snprintf(long_config_path, sizeof(long_config_path),
             "/tmp/astv-test-%ld-long.conf", (long)getpid());
    {
        FILE *long_config = fopen(long_config_path, "w");
        int i;

        assert(long_config != NULL);
        fputs("host=", long_config);
        for (i = 0; i < 1100; i++)
            fputc('a', long_config);
        fputs("\nport=5038\nusername=test\nsecret=test\n", long_config);
        assert(fclose(long_config) == 0);
    }
    astv_config_defaults(&loaded);
    assert(astv_config_load(long_config_path, &loaded, error,
                            sizeof(error)) < 0);

    unlink(symlink_path);
    unlink(config_path);
    unlink(long_config_path);

    puts("tests passed");
    return 0;
}
