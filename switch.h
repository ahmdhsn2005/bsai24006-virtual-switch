#ifndef SWITCH_H
#define SWITCH_H

#include "queue.h"
#include "mac_table.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define SWITCH_MAX_PORTS 16

#define MAC_AGE_SECONDS 300

#define PORT_RATE_FRAMES_PER_SECOND 100.0

typedef struct switch_state switch_state_t;

typedef struct
{
    int id;

    int fd;

    int active;

    pthread_t receiver_thread;

    pthread_t sender_thread;

    int receiver_started;

    int sender_started;

    atomic_int receiver_done;

    output_queue_t output;

    switch_state_t *sw;

    unsigned long frames_in;

    unsigned long frames_out;

    unsigned long frames_dropped;

} switch_port_t;

struct switch_state
{
    int server_fd;

    int max_ports;

    switch_port_t ports[SWITCH_MAX_PORTS];

    pthread_mutex_t ports_lock;

    sem_t free_ports;

    mac_table_t mac_table;

    pthread_t aging_thread;

    pthread_t refill_thread;

    atomic_int stopping;

    pthread_mutex_t timer_lock;

    pthread_cond_t timer_changed;
};

#endif