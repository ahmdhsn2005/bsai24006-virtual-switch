#ifndef ADMIN_H
#define ADMIN_H

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/un.h>

#define SWITCH_MAX_PORTS 16
#define STATS_NAME_SIZE 64
#define ADMIN_COMMAND_SIZE 128

typedef struct switch_state switch_state_t;

typedef struct
{
    unsigned long frames_in;

    unsigned long frames_out;

    unsigned long frames_dropped;

    unsigned long frames_mirrored;

} port_stats_t;

typedef struct
{
    pthread_mutex_t lock;

    int running;

    int max_ports;

    port_stats_t ports[SWITCH_MAX_PORTS];

} switch_stats_t;

typedef struct
{
    int fd;

    pthread_t thread;

    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];

    char stats_name[STATS_NAME_SIZE];

    switch_stats_t *stats;

} admin_t;

int switch_path(char *path, size_t size, const char *name);

int admin_address(struct sockaddr_un *address);

int stats_lock(switch_stats_t *stats);

int stats_print(switch_stats_t *stats, FILE *output);

int admin_start(switch_state_t *sw);

void admin_stop(switch_state_t *sw);

#endif
