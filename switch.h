#ifndef SWITCH_H
#define SWITCH_H

#include "queue.h"
#include "mac_table.h"
#include "admin.h"
#include "logger.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <signal.h>

#define MAC_AGE_SECONDS 300

#define PORT_RATE_FRAMES_PER_SECOND 100U

typedef struct
{
    int vlan;

    int mirror;

    unsigned int rate;

} port_config_t;

typedef struct
{
    int id;

    int fd;

    int active;

    int enabled;

    pthread_t receiver_thread;

    pthread_t sender_thread;

    int receiver_started;

    output_queue_t output;

    switch_state_t *sw;

} switch_port_t;

struct switch_state
{
    int server_fd;

    int max_ports;

    int wake[2];

    int cancel_admission;

    switch_port_t ports[SWITCH_MAX_PORTS];

    port_config_t config[SWITCH_MAX_PORTS];

    pthread_rwlock_t ports_lock;

    sem_t free_ports;

    mac_table_t mac_table;

    pthread_t aging_thread;

    pthread_t refill_thread;

    pthread_t signal_thread;

    sigset_t signals;

    admin_t admin;

    logger_t log;

    atomic_int stopping;

    pthread_mutex_t timer_lock;

    pthread_cond_t timer_changed;
};

void switch_set_vlan(switch_state_t *sw, int id, int vlan);

int switch_shutdown_port(switch_state_t *sw, int id);

int switch_up_port(switch_state_t *sw, int id);

void switch_apply_config(switch_state_t *sw, const port_config_t *config);

int switch_reload_config(switch_state_t *sw, int initial);

#endif
