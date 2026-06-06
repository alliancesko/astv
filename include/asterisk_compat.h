#ifndef ASTV_ASTERISK_COMPAT_H
#define ASTV_ASTERISK_COMPAT_H

#include <stddef.h>

/* Numeric form used for version-dependent AMI compatibility decisions. */
struct astv_asterisk_version {
    int major;
    int minor;
    int patch;
    int known;
};

int astv_asterisk_version_parse(const char *text,
                                struct astv_asterisk_version *version);
int astv_asterisk_version_at_least(
    const struct astv_asterisk_version *version,
    int major, int minor, int patch);
int astv_asterisk_supports_pjsip_contacts(
    const struct astv_asterisk_version *version);
int astv_asterisk_supports_status_snapshot(
    const struct astv_asterisk_version *version);
size_t astv_pjsip_contact_count(const char *contacts);

#endif
