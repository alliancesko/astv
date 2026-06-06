#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Loading, validation, and crash-safe persistence of AMI credentials. */

static void set_error(char *error, size_t size, const char *format, ...)
{
    va_list arguments;

    if (size == 0)
        return;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static void copy_text(char *destination, size_t size, const char *source)
{
    size_t length = strlen(source);

    if (length >= size)
        length = size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return text;
}

void astv_config_defaults(struct astv_config *config)
{
    memset(config, 0, sizeof(*config));
    copy_text(config->host, sizeof(config->host), ASTV_DEFAULT_HOST);
    copy_text(config->port, sizeof(config->port), ASTV_DEFAULT_PORT);
    copy_text(config->username, sizeof(config->username), ASTV_DEFAULT_USER);
}

int astv_config_validate(const struct astv_config *config,
                         char *error, size_t error_size)
{
    char *end;
    long port;

    if (config->host[0] == '\0') {
        set_error(error, error_size, "host is empty");
        return -1;
    }
    errno = 0;
    port = strtol(config->port, &end, 10);
    if (errno != 0 || *end != '\0' || port < 1 || port > 65535) {
        set_error(error, error_size, "port must be between 1 and 65535");
        return -1;
    }
    if (config->username[0] == '\0') {
        set_error(error, error_size, "username is empty");
        return -1;
    }
    if (config->secret[0] == '\0') {
        set_error(error, error_size, "secret is empty");
        return -1;
    }
    return 0;
}

int astv_config_load(const char *path, struct astv_config *config,
                     char *error, size_t error_size)
{
    FILE *file;
    char line[1024];
    unsigned long line_number = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT)
            return 1;
        set_error(error, error_size, "cannot open %s: %s", path,
                  strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *key;
        char *value;
        char *separator;

        line_number++;
        /* Reject truncated input instead of parsing one long line in pieces. */
        if (strchr(line, '\n') == NULL && !feof(file)) {
            set_error(error, error_size, "%s:%lu: line is too long",
                      path, line_number);
            fclose(file);
            return -1;
        }
        key = trim(line);
        if (key[0] == '\0' || key[0] == '#')
            continue;
        separator = strchr(key, '=');
        if (separator == NULL) {
            set_error(error, error_size, "%s:%lu: expected key=value",
                      path, line_number);
            fclose(file);
            return -1;
        }
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);

        if (strcmp(key, "host") == 0)
            copy_text(config->host, sizeof(config->host), value);
        else if (strcmp(key, "port") == 0)
            copy_text(config->port, sizeof(config->port), value);
        else if (strcmp(key, "username") == 0)
            copy_text(config->username, sizeof(config->username), value);
        else if (strcmp(key, "secret") == 0)
            copy_text(config->secret, sizeof(config->secret), value);
    }

    if (ferror(file)) {
        set_error(error, error_size, "cannot read %s: %s", path,
                  strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);
    return astv_config_validate(config, error, error_size);
}

static int create_parent_directory(const char *path, char *error,
                                   size_t error_size)
{
    char directory[PATH_MAX];
    char *slash;

    if (strlen(path) >= sizeof(directory)) {
        set_error(error, error_size, "configuration path is too long");
        return -1;
    }
    copy_text(directory, sizeof(directory), path);
    slash = strrchr(directory, '/');
    if (slash == NULL || slash == directory)
        return 0;
    *slash = '\0';

    /* The default layout has one managed directory: /etc/astv. */
    if (mkdir(directory, 0755) < 0 && errno != EEXIST) {
        set_error(error, error_size, "cannot create %s: %s", directory,
                  strerror(errno));
        return -1;
    }
    return 0;
}

static int sync_parent_directory(const char *path)
{
    char directory[PATH_MAX];
    char *slash;
    int fd;
    int result;

    if (strlen(path) >= sizeof(directory))
        return -1;
    copy_text(directory, sizeof(directory), path);
    slash = strrchr(directory, '/');
    if (slash == NULL) {
        copy_text(directory, sizeof(directory), ".");
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    /* Persist the rename itself, not only the temporary file contents. */
    fd = open(directory, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -1;
    result = fsync(fd);
    close(fd);
    return result;
}

int astv_config_save(const char *path, const struct astv_config *config,
                     char *error, size_t error_size)
{
    struct stat status;
    char temporary[PATH_MAX];
    int fd;
    FILE *file;
    int write_failed;
    int close_failed;
    int saved_errno;
    int temporary_length;

    if (astv_config_validate(config, error, error_size) < 0)
        return -1;
    if (create_parent_directory(path, error, error_size) < 0)
        return -1;
    /* Do not allow a privileged first-run setup to follow a symlink. */
    if (lstat(path, &status) == 0) {
        if (S_ISLNK(status.st_mode)) {
            set_error(error, error_size,
                      "refusing to replace symbolic link %s", path);
            return -1;
        }
    } else if (errno != ENOENT) {
        set_error(error, error_size, "cannot inspect %s: %s", path,
                  strerror(errno));
        return -1;
    }
    temporary_length =
        snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (temporary_length < 0 ||
        (size_t)temporary_length >= sizeof(temporary)) {
        set_error(error, error_size, "configuration path is too long");
        return -1;
    }

    /* Write privately, fsync, then atomically replace the destination. */
    fd = mkstemp(temporary);
    if (fd < 0) {
        set_error(error, error_size, "cannot create temporary file for %s: %s",
                  path,
                  strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0600) < 0) {
        set_error(error, error_size, "cannot protect %s: %s", path,
                  strerror(errno));
        close(fd);
        unlink(temporary);
        return -1;
    }

    file = fdopen(fd, "w");
    if (file == NULL) {
        set_error(error, error_size, "cannot write %s: %s", path,
                  strerror(errno));
        close(fd);
        unlink(temporary);
        return -1;
    }
    write_failed =
        fprintf(file,
                "# astv Asterisk AMI connection\n"
                "host=%s\n"
                "port=%s\n"
                "username=%s\n"
                "secret=%s\n",
                config->host, config->port, config->username,
                config->secret) < 0;
    if (!write_failed && fflush(file) != 0)
        write_failed = 1;
    if (!write_failed && fsync(fd) != 0)
        write_failed = 1;
    close_failed = fclose(file) != 0;
    if (write_failed || close_failed) {
        saved_errno = errno;
        unlink(temporary);
        set_error(error, error_size, "cannot write %s: %s", path,
                  strerror(saved_errno));
        return -1;
    }
    if (rename(temporary, path) < 0) {
        saved_errno = errno;
        unlink(temporary);
        set_error(error, error_size, "cannot replace %s: %s", path,
                  strerror(saved_errno));
        return -1;
    }
    if (sync_parent_directory(path) < 0) {
        set_error(error, error_size,
                  "cannot synchronize directory for %s: %s",
                  path, strerror(errno));
        return -1;
    }
    return 0;
}
