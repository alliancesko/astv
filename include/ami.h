#ifndef ASTV_AMI_H
#define ASTV_AMI_H

#include <stddef.h>

#define AMI_MAX_FIELDS 128
#define AMI_KEY_SIZE 64
#define AMI_VALUE_SIZE 512
#define AMI_BUFFER_SIZE (128 * 1024)

/* A parsed AMI message is stored as bounded key/value fields. */
struct ami_field {
    char key[AMI_KEY_SIZE];
    char value[AMI_VALUE_SIZE];
};

struct ami_message {
    struct ami_field fields[AMI_MAX_FIELDS];
    size_t count;
    /* Set when valid fields beyond AMI_MAX_FIELDS were discarded. */
    int truncated;
};

typedef void (*ami_message_cb)(const struct ami_message *message, void *data);

/* Streaming parser state retained between socket reads. */
struct ami_parser {
    char buffer[AMI_BUFFER_SIZE];
    size_t length;
};

void ami_parser_init(struct ami_parser *parser);
int ami_parser_feed(struct ami_parser *parser, const char *data, size_t length,
                    ami_message_cb callback, void *callback_data);
const char *ami_get(const struct ami_message *message, const char *key);

#endif
