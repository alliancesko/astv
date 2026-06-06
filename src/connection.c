#define _POSIX_C_SOURCE 200809L

#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Bounded DNS/TCP connection attempts executed outside the TUI main loop. */

struct worker_args {
    struct astv_connect_job *job;
    int timeout_ms;
};

struct connect_cleanup {
    struct addrinfo *addresses;
    int fd;
};

static void cleanup_connect(void *data)
{
    struct connect_cleanup *cleanup = data;

    /* pthread cancellation must not leak an in-progress socket or addrinfo. */
    if (cleanup->fd >= 0)
        close(cleanup->fd);
    if (cleanup->addresses != NULL)
        freeaddrinfo(cleanup->addresses);
}

static long long monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (long long)now.tv_sec * 1000LL +
           (long long)now.tv_nsec / 1000000LL;
}

static void copy_text(char *destination, size_t size, const char *source)
{
    size_t length = strlen(source);

    if (length >= size)
        length = size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

int astv_connect_server(const char *host, const char *port,
                        char *error_text, size_t error_size,
                        int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *item;
    struct connect_cleanup cleanup = {NULL, -1};
    volatile int fd = -1;
    int error;
    volatile int last_errno = 0;
    long long deadline = monotonic_milliseconds() + timeout_ms;

    pthread_cleanup_push(cleanup_connect, &cleanup);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(host, port, &hints, &cleanup.addresses);
    if (error != 0) {
        snprintf(error_text, error_size, "cannot resolve %s:%s: %s",
                 host, port, gai_strerror(error));
        goto done;
    }

    /* Try every resolved IPv4/IPv6 address within one shared deadline. */
    for (item = cleanup.addresses; item != NULL; item = item->ai_next) {
        int flags;

        if (monotonic_milliseconds() >= deadline) {
            last_errno = ETIMEDOUT;
            break;
        }
        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0)
            continue;
        cleanup.fd = fd;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_errno = errno;
            close(fd);
            fd = -1;
            cleanup.fd = -1;
            continue;
        }
        /* Nonblocking connect keeps the timeout under application control. */
        if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            if (fcntl(fd, F_SETFL, flags) == 0)
                break;
            last_errno = errno;
        } else if (errno == EINPROGRESS) {
            struct pollfd descriptor = {fd, POLLOUT, 0};
            int poll_result;

            do {
                long long remaining =
                    deadline - monotonic_milliseconds();

                if (remaining <= 0) {
                    poll_result = 0;
                    break;
                }
                poll_result = poll(&descriptor, 1, (int)remaining);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result > 0) {
                socklen_t length = sizeof(error);

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error,
                               &length) == 0 &&
                    error == 0 && fcntl(fd, F_SETFL, flags) == 0)
                    break;
                last_errno = error != 0 ? error : errno;
            } else {
                last_errno = poll_result == 0 ? ETIMEDOUT : errno;
            }
        } else {
            last_errno = errno;
        }
        close(fd);
        fd = -1;
        cleanup.fd = -1;
    }
    freeaddrinfo(cleanup.addresses);
    cleanup.addresses = NULL;
    if (fd < 0)
        snprintf(error_text, error_size, "cannot connect to %s:%s: %s",
                 host, port,
                 strerror(last_errno != 0 ? last_errno : errno));
done:
    cleanup.fd = -1;
    if (cleanup.addresses != NULL) {
        freeaddrinfo(cleanup.addresses);
        cleanup.addresses = NULL;
    }
    pthread_cleanup_pop(0);
    return fd;
}

static void *connect_worker(void *data)
{
    struct worker_args *arguments = data;
    struct astv_connect_job *job = arguments->job;
    int timeout_ms = arguments->timeout_ms;

    free(arguments);
    job->fd = astv_connect_server(job->host, job->port,
                                  job->error, sizeof(job->error),
                                  timeout_ms);
    /* Release publishes fd and error text to the polling main thread. */
    atomic_store_explicit(&job->done, 1, memory_order_release);
    return NULL;
}

int astv_connect_job_start(struct astv_connect_job *job,
                           const char *host, const char *port,
                           int timeout_ms)
{
    struct worker_args *arguments;
    int error;

    if (job->active)
        return 0;
    arguments = malloc(sizeof(*arguments));
    if (arguments == NULL) {
        snprintf(job->error, sizeof(job->error),
                 "cannot allocate reconnect worker");
        return -1;
    }
    copy_text(job->host, sizeof(job->host), host);
    copy_text(job->port, sizeof(job->port), port);
    job->fd = -1;
    job->error[0] = '\0';
    atomic_store_explicit(&job->done, 0, memory_order_relaxed);
    arguments->job = job;
    arguments->timeout_ms = timeout_ms;
    error = pthread_create(&job->thread, NULL, connect_worker, arguments);
    if (error != 0) {
        free(arguments);
        snprintf(job->error, sizeof(job->error),
                 "cannot start reconnect worker: %s", strerror(error));
        return -1;
    }
    job->active = 1;
    return 0;
}

int astv_connect_job_finished(const struct astv_connect_job *job)
{
    /* Acquire pairs with the worker's release after all result writes. */
    return job->active &&
           atomic_load_explicit(&job->done, memory_order_acquire);
}

void astv_connect_job_finish(struct astv_connect_job *job)
{
    if (!job->active)
        return;
    pthread_join(job->thread, NULL);
    job->active = 0;
}

void astv_connect_job_cancel(struct astv_connect_job *job)
{
    if (!job->active)
        return;
    pthread_cancel(job->thread);
    pthread_join(job->thread, NULL);
    job->active = 0;
    job->fd = -1;
}
