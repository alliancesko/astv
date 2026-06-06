#include "ami.h"

#include <string.h>
#include <strings.h>

/* AMI framing and bounded key/value parsing. */

void ami_parser_init(struct ami_parser *parser)
{
    parser->length = 0;
}

static const char *find_message_end(const char *buffer, size_t length,
                                    size_t *delimiter_length)
{
    size_t i;

    for (i = 0; i + 3 < length; i++) {
        if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
            *delimiter_length = 4;
            return buffer + i;
        }
    }

    /* Accept LF-only framing used by some proxies and test fixtures. */
    for (i = 0; i + 1 < length; i++) {
        if (memcmp(buffer + i, "\n\n", 2) == 0) {
            *delimiter_length = 2;
            return buffer + i;
        }
    }

    return NULL;
}

static void copy_string(char *destination, size_t size, const char *source,
                        size_t length)
{
    if (length >= size)
        length = size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void parse_message(const char *text, size_t length,
                          struct ami_message *message)
{
    size_t position = 0;

    message->count = 0;
    message->truncated = 0;
    while (position < length) {
        const char *line = text + position;
        const char *newline = memchr(line, '\n', length - position);
        const char *colon;
        size_t line_length = newline ? (size_t)(newline - line)
                                     : length - position;
        size_t value_offset;

        if (line_length > 0 && line[line_length - 1] == '\r')
            line_length--;

        /* Unknown keys are retained; consumers choose fields by name. */
        colon = memchr(line, ':', line_length);
        if (colon != NULL && message->count < AMI_MAX_FIELDS) {
            struct ami_field *field = &message->fields[message->count++];
            size_t key_length = (size_t)(colon - line);

            value_offset = key_length + 1;
            while (value_offset < line_length &&
                   (line[value_offset] == ' ' || line[value_offset] == '\t'))
                value_offset++;

            copy_string(field->key, sizeof(field->key), line, key_length);
            copy_string(field->value, sizeof(field->value),
                        line + value_offset, line_length - value_offset);
        } else if (colon != NULL) {
            message->truncated = 1;
        }

        if (newline == NULL)
            break;
        position = (size_t)(newline - text) + 1;
    }
}

int ami_parser_feed(struct ami_parser *parser, const char *data, size_t length,
                    ami_message_cb callback, void *callback_data)
{
    size_t consumed_total = 0;

    if (length > sizeof(parser->buffer) - parser->length) {
        parser->length = 0;
        return -1;
    }

    memcpy(parser->buffer + parser->length, data, length);
    parser->length += length;

    /* Parse every complete frame and compact the buffer only once. */
    for (;;) {
        struct ami_message message;
        const char *end;
        size_t delimiter_length;
        size_t message_length;
        size_t consumed;

        end = find_message_end(parser->buffer + consumed_total,
                               parser->length - consumed_total,
                               &delimiter_length);
        if (end == NULL)
            break;

        message_length =
            (size_t)(end - (parser->buffer + consumed_total));
        parse_message(parser->buffer + consumed_total,
                      message_length, &message);
        if (message.count > 0)
            callback(&message, callback_data);

        consumed = message_length + delimiter_length;
        consumed_total += consumed;
    }
    if (consumed_total > 0) {
        memmove(parser->buffer, parser->buffer + consumed_total,
                parser->length - consumed_total);
        parser->length -= consumed_total;
    }

    return 0;
}

const char *ami_get(const struct ami_message *message, const char *key)
{
    size_t i;

    for (i = 0; i < message->count; i++) {
        if (strcasecmp(message->fields[i].key, key) == 0)
            return message->fields[i].value;
    }
    return "";
}
