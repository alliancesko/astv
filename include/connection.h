#ifndef ASTV_CONNECTION_H
#define ASTV_CONNECTION_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

/* One asynchronous DNS/TCP connection attempt owned by the main loop. */
struct astv_connect_job {
    pthread_t thread;
    atomic_int done;
    int active;
    int fd;
    char host[256];
    char port[16];
    char error[256];
};

int astv_connect_server(const char *host, const char *port,
                        char *error_text, size_t error_size,
                        int timeout_ms);
int astv_connect_job_start(struct astv_connect_job *job,
                           const char *host, const char *port,
                           int timeout_ms);
int astv_connect_job_finished(const struct astv_connect_job *job);
void astv_connect_job_finish(struct astv_connect_job *job);
void astv_connect_job_cancel(struct astv_connect_job *job);

#endif
