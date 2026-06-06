#include "asterisk_compat.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Compatibility probes for AMI differences between Asterisk releases. */

static int parse_component(const char **cursor, int *value)
{
    char *end;
    long parsed;

    if (!isdigit((unsigned char)**cursor))
        return -1;
    parsed = strtol(*cursor, &end, 10);
    if (parsed < 0 || parsed > INT_MAX)
        return -1;
    *value = (int)parsed;
    *cursor = end;
    return 0;
}

int astv_asterisk_version_parse(const char *text,
                                struct astv_asterisk_version *version)
{
    const char *cursor = text;
    int best_components = 0;

    memset(version, 0, sizeof(*version));
    /*
     * Version banners may contain branch numbers before the actual release.
     * Prefer the candidate with the most numeric components.
     */
    while (*cursor != '\0') {
        struct astv_asterisk_version candidate = {0};
        const char *candidate_cursor;
        int components = 1;

        while (*cursor != '\0' && !isdigit((unsigned char)*cursor))
            cursor++;
        if (*cursor == '\0')
            break;
        candidate_cursor = cursor;
        if (parse_component(&candidate_cursor, &candidate.major) < 0) {
            cursor++;
            continue;
        }
        if (*candidate_cursor == '.') {
            candidate_cursor++;
            if (parse_component(&candidate_cursor,
                                &candidate.minor) == 0) {
                components = 2;
                if (*candidate_cursor == '.') {
                    candidate_cursor++;
                    if (parse_component(&candidate_cursor,
                                        &candidate.patch) == 0)
                        components = 3;
                }
            }
        }
        if (components > best_components) {
            *version = candidate;
            best_components = components;
        }
        cursor = candidate_cursor;
    }
    if (best_components == 0)
        return -1;
    version->known = 1;
    return 0;
}

int astv_asterisk_version_at_least(
    const struct astv_asterisk_version *version,
    int major, int minor, int patch)
{
    if (!version->known)
        return 0;
    if (version->major != major)
        return version->major > major;
    if (version->minor != minor)
        return version->minor > minor;
    return version->patch >= patch;
}

int astv_asterisk_supports_pjsip_contacts(
    const struct astv_asterisk_version *version)
{
    return !version->known ||
           astv_asterisk_version_at_least(version, 16, 0, 0);
}

int astv_asterisk_supports_status_snapshot(
    const struct astv_asterisk_version *version)
{
    /*
     * The Status action fields used by astv appeared in different point
     * releases of the supported Asterisk branches.
     */
    if (!version->known)
        return 1;
    if (version->major == 18)
        return astv_asterisk_version_at_least(version, 18, 26, 0);
    if (version->major == 20)
        return astv_asterisk_version_at_least(version, 20, 11, 0);
    if (version->major == 21)
        return astv_asterisk_version_at_least(version, 21, 6, 0);
    if (version->major == 22)
        return astv_asterisk_version_at_least(version, 22, 1, 0);
    return version->major > 22;
}

size_t astv_pjsip_contact_count(const char *contacts)
{
    size_t count = 0;
    const char *cursor = contacts;

    /* Count URI occurrences in the compact Contacts field. */
    while (*cursor != '\0') {
        if ((cursor == contacts ||
             !isalnum((unsigned char)cursor[-1])) &&
            (strncasecmp(cursor, "sip:", 4) == 0 ||
             strncasecmp(cursor, "sips:", 5) == 0)) {
            count++;
            cursor += strncasecmp(cursor, "sips:", 5) == 0 ? 5 : 4;
        } else {
            cursor++;
        }
    }
    /* Non-empty legacy values without an explicit URI represent one contact. */
    if (count == 0) {
        cursor = contacts;
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (*cursor != '\0' && strcmp(cursor, "-") != 0 &&
            strcmp(cursor, "<none>") != 0)
            count = 1;
    }
    return count;
}
