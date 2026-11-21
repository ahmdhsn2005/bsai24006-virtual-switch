#define _GNU_SOURCE

#include "switch.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

static int is_broadcast(const uint8_t mac[6])
{
    static const uint8_t broadcast[6] = {
        255, 255, 255, 255, 255, 255
    };

    return memcmp(mac, broadcast, 6) == 0;
}

static void shutdown_socket(int fd)
{
    if (shutdown(fd, SHUT_RDWR) < 0 && errno != ENOTCONN)
        perror("shutdown");
}

static int enqueue_to_port(
    switch_port_t *port,
    shared_frame_t *frame
)
{
    shared_frame_retain(frame);

    if (output_queue_push(&port->output, frame) == 0)
        return 0;

    shared_frame_release(frame);

    switch_stats_t *stats = port->sw->admin.stats;

    if (stats_lock(stats) == 0)
    {
        stats->ports[port->id].frames_dropped++;

        pthread_mutex_unlock(&stats->lock);
    }

    return -1;
}

static void mirror_frame(
    switch_state_t *sw,
    int id,
    shared_frame_t *frame
)
{
    int mirror = sw->config[id].mirror;

    if (mirror >= 0 && sw->ports[mirror].enabled &&
        sw->ports[mirror].active &&
        enqueue_to_port(&sw->ports[mirror], frame) == 0)
    {
        switch_stats_t *stats = sw->admin.stats;

        if (stats_lock(stats) == 0)
        {
            stats->ports[id].frames_mirrored++;

            pthread_mutex_unlock(&stats->lock);
        }
    }
}

static int forward_frame(
    switch_port_t *source,
    frame_t *frame
)
{
    switch_state_t *sw = source->sw;

    pthread_rwlock_rdlock(&sw->ports_lock);

    if (!source->enabled || atomic_load(&sw->stopping))
    {
        pthread_rwlock_unlock(&sw->ports_lock);

        return -1;
    }

    int vlan = sw->config[source->id].vlan;

    if (vlan != 0)
        frame->vlan = (uint16_t)vlan;

    int previous_port;

    int learned = mac_table_learn_vlan(
        &sw->mac_table,
        frame->src,
        frame->vlan,
        source->id,
        &previous_port
    );

    if (learned < 0)
    {
        pthread_rwlock_unlock(&sw->ports_lock);

        return learned == -2 ? -2 : 0;
    }

    if (previous_port != source->id)
    {
        logger_write(&sw->log, "MAC moved: port %d -> port %d, VLAN %u",
                     previous_port, source->id, frame->vlan);
    }

    shared_frame_t *shared = shared_frame_create(frame);

    if (shared == NULL)
    {
        pthread_rwlock_unlock(&sw->ports_lock);

        return 0;
    }

    int destination = -1;

    if (!is_broadcast(frame->dst))
        mac_table_lookup_vlan(&sw->mac_table, frame->dst, frame->vlan, &destination);

    mirror_frame(sw, source->id, shared);

    for (int i = 0; i < sw->max_ports; i++)
    {
        if (i != source->id &&
            sw->ports[i].enabled &&
            sw->ports[i].active &&
            (destination < 0 || i == destination) &&
            (sw->config[i].vlan == 0 || sw->config[i].vlan == frame->vlan) &&
            enqueue_to_port(&sw->ports[i], shared) == 0)
        {
            mirror_frame(sw, i, shared);
        }
    }

    pthread_rwlock_unlock(&sw->ports_lock);

    shared_frame_release(shared);

    return 0;
}

static void *receiver_main(void *argument)
{
    switch_port_t *port = argument;

    switch_state_t *sw = port->sw;

    switch_stats_t *stats = sw->admin.stats;

    frame_t frame;

    int forwarded = 0;

    while (!atomic_load(&sw->stopping))
    {
        int result = frame_read(port->fd, &frame);

        if (result != 1)
            break;

        if (stats_lock(stats) == 0)
        {
            stats->ports[port->id].frames_in++;

            pthread_mutex_unlock(&stats->lock);
        }

        forwarded = forward_frame(port, &frame);

        if (forwarded < 0)
            break;
    }

    pthread_rwlock_wrlock(&sw->ports_lock);

    if (forwarded == -2)
    {
        port->enabled = 0;

        mac_table_remove_port(&sw->mac_table, port->id);

        logger_write(&sw->log,
                     "Port %d exceeded MAC limit and is down until reenabled",
                     port->id);
    }

    if (forwarded == -2 || port->enabled || atomic_load(&sw->stopping))
    {
        shutdown_socket(port->fd);

        output_queue_close(&port->output);
    }

    pthread_rwlock_unlock(&sw->ports_lock);

    pthread_join(port->sender_thread, NULL);

    pthread_rwlock_wrlock(&sw->ports_lock);

    mac_table_remove_port(&sw->mac_table, port->id);

    if (close(port->fd) < 0)
        perror("close");

    size_t dropped = output_queue_configure(&port->output,
                                            sw->config[port->id].rate, 1);

    if (dropped > 0 && stats_lock(stats) == 0)
    {
        stats->ports[port->id].frames_dropped += dropped;

        pthread_mutex_unlock(&stats->lock);
    }

    output_queue_destroy(&port->output);

    port->active = 0;

    logger_write(&sw->log, "Port %d disconnected", port->id);

    if (port->enabled && sem_post(&sw->free_ports) < 0)
        perror("sem_post");

    pthread_rwlock_unlock(&sw->ports_lock);

    return NULL;
}

static void *sender_main(void *argument)
{
    switch_port_t *port = argument;

    switch_stats_t *stats = port->sw->admin.stats;

    for (;;)
    {
        unsigned long generation;

        shared_frame_t *shared =
            output_queue_pop(&port->output, &generation);

        if (shared == NULL)
            break;

        int current = output_queue_current(&port->output, generation);

        int result = current ? frame_write(port->fd, &shared->frame) : 0;

        shared_frame_release(shared);

        if (stats_lock(stats) == 0)
        {
            if (current && result == 0)
                stats->ports[port->id].frames_out++;
            else
                stats->ports[port->id].frames_dropped++;

            pthread_mutex_unlock(&stats->lock);
        }

        if (result < 0)
            break;
    }

    shutdown_socket(port->fd);

    return NULL;
}

static void *aging_main(void *argument)
{
    switch_state_t *sw = argument;

    while (!atomic_load(&sw->stopping))
    {
        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
        {
            perror("clock_gettime");

            break;
        }

        deadline.tv_sec += 5;

        pthread_mutex_lock(&sw->timer_lock);

        int result = 0;

        if (!atomic_load(&sw->stopping))
        {
            result = pthread_cond_timedwait(
                &sw->timer_changed,
                &sw->timer_lock,
                &deadline
            );
        }

        pthread_mutex_unlock(&sw->timer_lock);

        if (result != 0 && result != ETIMEDOUT)
        {
            logger_write(&sw->log, "Timer wait failed: %s", strerror(result));

            break;
        }

        if (!atomic_load(&sw->stopping))
        {
            mac_table_age(
                &sw->mac_table,
                MAC_AGE_SECONDS
            );
        }
    }

    return NULL;
}

static void *refill_main(void *argument)
{
    switch_state_t *sw = argument;

    while (!atomic_load(&sw->stopping))
    {
        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
        {
            perror("clock_gettime");

            break;
        }

        deadline.tv_nsec += 100000000;

        if (deadline.tv_nsec >= 1000000000)
        {
            deadline.tv_sec++;

            deadline.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&sw->timer_lock);

        int result = 0;

        if (!atomic_load(&sw->stopping))
        {
            result = pthread_cond_timedwait(
                &sw->timer_changed,
                &sw->timer_lock,
                &deadline
            );
        }

        pthread_mutex_unlock(&sw->timer_lock);

        if (result != 0 && result != ETIMEDOUT)
        {
            logger_write(&sw->log, "Timer wait failed: %s", strerror(result));

            break;
        }

        pthread_rwlock_rdlock(&sw->ports_lock);

        for (int i = 0; i < sw->max_ports; i++)
        {
            if (sw->ports[i].active)
            {
                output_queue_refill(
                    &sw->ports[i].output
                );
            }
        }

        pthread_rwlock_unlock(&sw->ports_lock);
    }

    return NULL;
}

static int start_port(
    switch_state_t *sw,
    int fd,
    int id
)
{
    switch_port_t *port = &sw->ports[id];

    if (port->receiver_started)
        pthread_join(port->receiver_thread, NULL);

    port->receiver_started = 0;

    port->id = id;

    port->fd = fd;

    port->sw = sw;

    if (output_queue_init(
            &port->output,
            sw->config[id].rate
        ) < 0)
    {
        return -1;
    }

    if (pthread_create(
            &port->sender_thread,
            NULL,
            sender_main,
            port
        ) != 0)
    {
        output_queue_destroy(&port->output);

        return -1;
    }

    if (pthread_create(
            &port->receiver_thread,
            NULL,
            receiver_main,
            port
        ) != 0)
    {
        output_queue_close(&port->output);

        shutdown_socket(fd);

        pthread_join(port->sender_thread, NULL);

        output_queue_destroy(&port->output);

        return -1;
    }

    port->receiver_started = 1;

    port->active = 1;

    logger_write(&sw->log, "Host connected on switch port %d", id);

    return 0;
}

static int create_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;

    int yes = 1;

    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &yes,
            sizeof(yes)
        ) < 0)
    {
        close(fd);

        return -1;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;

    address.sin_addr.s_addr = htonl(INADDR_ANY);

    address.sin_port = htons((uint16_t)port);

    if (bind(
            fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0)
    {
        close(fd);

        return -1;
    }

    if (listen(fd, SWITCH_MAX_PORTS) < 0)
    {
        close(fd);

        return -1;
    }

    return fd;
}

static void configure_port(switch_state_t *sw, int id, port_config_t config)
{
    port_config_t old = sw->config[id];

    int changed_vlan = old.vlan != config.vlan;

    sw->config[id] = config;

    if (changed_vlan)
        mac_table_remove_port(&sw->mac_table, id);

    if (sw->ports[id].active && (changed_vlan || old.rate != config.rate))
    {
        size_t dropped = output_queue_configure(
            &sw->ports[id].output,
            config.rate,
            changed_vlan
        );

        if (dropped > 0 && stats_lock(sw->admin.stats) == 0)
        {
            sw->admin.stats->ports[id].frames_dropped += dropped;

            pthread_mutex_unlock(&sw->admin.stats->lock);
        }
    }
}

void switch_apply_config(switch_state_t *sw, const port_config_t *config)
{
    pthread_rwlock_wrlock(&sw->ports_lock);

    for (int i = 0; i < sw->max_ports; i++)
        configure_port(sw, i, config[i]);

    pthread_rwlock_unlock(&sw->ports_lock);
}

void switch_set_vlan(switch_state_t *sw, int id, int vlan)
{
    pthread_rwlock_wrlock(&sw->ports_lock);

    if (vlan == 0)
    {
        for (int i = 0; i < sw->max_ports; i++)
        {
            if (i != id && sw->config[i].vlan == 0)
            {
                port_config_t access = sw->config[i];

                access.vlan = 1;

                configure_port(sw, i, access);
            }
        }
    }

    port_config_t config = sw->config[id];

    config.vlan = vlan;

    configure_port(sw, id, config);

    pthread_rwlock_unlock(&sw->ports_lock);
}

int switch_shutdown_port(switch_state_t *sw, int id)
{
    switch_port_t *port = &sw->ports[id];

    pthread_rwlock_wrlock(&sw->ports_lock);

    if (port->enabled)
    {
        if (!port->active)
        {
            int result;

            do
            {
                result = sem_trywait(&sw->free_ports);
            }
            while (result < 0 && errno == EINTR);

            if (result < 0 && errno != EAGAIN)
            {
                pthread_rwlock_unlock(&sw->ports_lock);

                return -1;
            }

            if (result < 0)
                sw->cancel_admission = 1;
        }
        else
        {
            mac_table_remove_port(&sw->mac_table, id);

            output_queue_finish(&port->output);

            if (shutdown(port->fd, SHUT_RD) < 0 && errno != ENOTCONN)
                perror("shutdown");
        }

        port->enabled = 0;
    }

    int started = port->receiver_started;

    pthread_rwlock_unlock(&sw->ports_lock);

    if (started)
    {
        struct timespec timeout;

        int result = -1;

        if (clock_gettime(CLOCK_REALTIME, &timeout) == 0)
        {
            timeout.tv_sec += 2;

            result = pthread_timedjoin_np(port->receiver_thread, NULL, &timeout);
        }

        if (result != 0)
        {
            pthread_rwlock_wrlock(&sw->ports_lock);

            if (port->active)
            {
                output_queue_close(&port->output);

                shutdown_socket(port->fd);
            }

            pthread_rwlock_unlock(&sw->ports_lock);

            result = pthread_join(port->receiver_thread, NULL);

            if (result != 0)
            {
                errno = result;

                return -1;
            }
        }

        pthread_rwlock_wrlock(&sw->ports_lock);

        port->receiver_started = 0;

        pthread_rwlock_unlock(&sw->ports_lock);
    }

    logger_write(&sw->log, "Port %d shut down", id);

    return 0;
}

int switch_up_port(switch_state_t *sw, int id)
{
    pthread_rwlock_wrlock(&sw->ports_lock);

    switch_port_t *port = &sw->ports[id];

    if (port->enabled)
    {
        pthread_rwlock_unlock(&sw->ports_lock);

        return 0;
    }

    if (port->receiver_started)
    {
        pthread_t receiver = port->receiver_thread;

        pthread_rwlock_unlock(&sw->ports_lock);

        int result = pthread_join(receiver, NULL);

        if (result != 0)
        {
            errno = result;

            return -1;
        }

        pthread_rwlock_wrlock(&sw->ports_lock);

        port->receiver_started = 0;
    }

    if (sem_post(&sw->free_ports) < 0)
    {
        pthread_rwlock_unlock(&sw->ports_lock);

        return -1;
    }

    port->enabled = 1;

    pthread_rwlock_unlock(&sw->ports_lock);

    logger_write(&sw->log, "Port %d enabled", id);

    return 0;
}

static void request_stop(switch_state_t *sw)
{
    if (atomic_exchange(&sw->stopping, 1))
        return;

    ssize_t result;

    do
    {
        result = write(sw->wake[1], "", 1);
    }
    while (result < 0 && errno == EINTR);

    if (result < 0)
        perror("write");

    if (sem_post(&sw->free_ports) < 0)
        perror("sem_post");

    pthread_mutex_lock(&sw->timer_lock);

    pthread_cond_broadcast(&sw->timer_changed);

    pthread_mutex_unlock(&sw->timer_lock);
}

static void dump_mac(switch_state_t *sw)
{
    char path[1024];

    if (switch_path(path, sizeof(path), "mac_table.txt") < 0)
    {
        logger_write(&sw->log, "Could not build MAC dump path");

        return;
    }

    char *data = NULL;

    size_t length = 0;

    FILE *snapshot = open_memstream(&data, &length);

    if (snapshot == NULL)
    {
        logger_write(&sw->log, "Could not prepare MAC table dump");

        return;
    }

    int result = mac_table_print(&sw->mac_table, snapshot);

    if (fclose(snapshot) == EOF)
        result = -1;

    if (result == 0)
    {
        FILE *file = fopen(path, "w");

        if (file == NULL)
        {
            result = -1;
        }
        else
        {
            if (fwrite(data, 1, length, file) != length)
                result = -1;

            if (fclose(file) == EOF)
                result = -1;
        }
    }

    free(data);

    logger_write(&sw->log, result == 0 ? "MAC table dumped" : "MAC table dump failed");
}

static void *signal_main(void *argument)
{
    switch_state_t *sw = argument;

    while (!atomic_load(&sw->stopping))
    {
        int signal_number;

        int result = sigwait(&sw->signals, &signal_number);

        if (result != 0)
        {
            logger_write(&sw->log, "sigwait failed: %s", strerror(result));

            request_stop(sw);

            break;
        }

        if (signal_number == SIGINT || signal_number == SIGTERM)
        {
            logger_write(&sw->log, "Switch shutting down");

            request_stop(sw);
        }
        else if (signal_number == SIGHUP)
        {
            switch_reload_config(sw, 0);
        }
        else if (signal_number == SIGUSR1)
        {
            dump_mac(sw);
        }
    }

    return NULL;
}

static int init_switch(
    switch_state_t *sw,
    int tcp_port,
    int max_ports
)
{
    memset(sw, 0, sizeof(*sw));

    sw->max_ports = max_ports;

    atomic_init(&sw->stopping, 0);

    if (sigemptyset(&sw->signals) < 0 ||
        sigaddset(&sw->signals, SIGINT) < 0 ||
        sigaddset(&sw->signals, SIGTERM) < 0 ||
        sigaddset(&sw->signals, SIGHUP) < 0 ||
        sigaddset(&sw->signals, SIGUSR1) < 0 ||
        pthread_sigmask(SIG_BLOCK, &sw->signals, NULL) != 0)
    {
        return -1;
    }

    if (pipe(sw->wake) < 0)
        return -1;

    pthread_rwlockattr_t attributes;

    if (pthread_rwlockattr_init(&attributes) != 0)
        goto close_pipe;

    int result = pthread_rwlockattr_setkind_np(
        &attributes,
        PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
    );

    if (result == 0)
        result = pthread_rwlock_init(&sw->ports_lock, &attributes);

    pthread_rwlockattr_destroy(&attributes);

    if (result != 0)
        goto close_pipe;

    if (pthread_mutex_init(&sw->timer_lock, NULL) != 0)
        goto destroy_ports_lock;

    if (pthread_cond_init(&sw->timer_changed, NULL) != 0)
        goto destroy_timer_lock;

    if (sem_init(&sw->free_ports, 0, (unsigned int)max_ports) < 0)
        goto destroy_timer_cond;

    if (mac_table_init(&sw->mac_table) < 0)
        goto destroy_semaphore;

    for (int i = 0; i < max_ports; i++)
    {
        sw->ports[i].fd = -1;

        sw->ports[i].id = i;

        sw->ports[i].enabled = 1;
    }

    sw->server_fd = create_server(tcp_port);

    if (sw->server_fd < 0)
        goto destroy_mac;

    char path[1024];

    if (switch_path(path, sizeof(path), "switch.log") < 0 ||
        logger_start(&sw->log, path) < 0)
    {
        goto close_server;
    }

    if (switch_reload_config(sw, 1) < 0)
        goto stop_logger;

    if (pthread_create(&sw->aging_thread, NULL, aging_main, sw) != 0)
        goto stop_logger;

    if (pthread_create(&sw->refill_thread, NULL, refill_main, sw) != 0)
        goto stop_aging;

    if (pthread_create(&sw->signal_thread, NULL, signal_main, sw) != 0)
        goto stop_refill;

    if (admin_start(sw) < 0)
        goto stop_signals;

    logger_write(&sw->log, "Switch listening on TCP port %d with %d ports",
                 tcp_port, max_ports);

    return 0;

stop_signals:
    request_stop(sw);

    pthread_kill(sw->signal_thread, SIGINT);

    pthread_join(sw->signal_thread, NULL);
stop_refill:
    request_stop(sw);

    pthread_join(sw->refill_thread, NULL);
stop_aging:
    request_stop(sw);

    pthread_join(sw->aging_thread, NULL);
stop_logger:
    logger_stop(&sw->log);
close_server:
    close(sw->server_fd);
destroy_mac:
    mac_table_destroy(&sw->mac_table);
destroy_semaphore:
    sem_destroy(&sw->free_ports);
destroy_timer_cond:
    pthread_cond_destroy(&sw->timer_changed);
destroy_timer_lock:
    pthread_mutex_destroy(&sw->timer_lock);
destroy_ports_lock:
    pthread_rwlock_destroy(&sw->ports_lock);
close_pipe:
    close(sw->wake[0]);

    close(sw->wake[1]);

    return -1;
}

static void destroy_switch(switch_state_t *sw)
{
    request_stop(sw);

    close(sw->server_fd);

    pthread_rwlock_wrlock(&sw->ports_lock);

    for (int i = 0; i < sw->max_ports; i++)
    {
        switch_port_t *port = &sw->ports[i];

        if (port->active)
        {
            output_queue_close(&port->output);

            shutdown_socket(port->fd);
        }
    }

    pthread_rwlock_unlock(&sw->ports_lock);

    pthread_kill(sw->signal_thread, SIGINT);

    pthread_join(sw->signal_thread, NULL);

    pthread_join(sw->admin.thread, NULL);

    pthread_join(sw->aging_thread, NULL);

    pthread_join(sw->refill_thread, NULL);

    for (int i = 0; i < sw->max_ports; i++)
    {
        switch_port_t *port = &sw->ports[i];

        if (port->receiver_started)
            pthread_join(port->receiver_thread, NULL);
    }

    admin_stop(sw);

    mac_table_destroy(&sw->mac_table);

    sem_destroy(&sw->free_ports);

    pthread_cond_destroy(&sw->timer_changed);

    pthread_mutex_destroy(&sw->timer_lock);

    pthread_rwlock_destroy(&sw->ports_lock);

    close(sw->wake[0]);

    close(sw->wake[1]);

    logger_stop(&sw->log);
}

static int admit_host(switch_state_t *sw, int fd)
{
    struct timespec timeout;

    if (clock_gettime(CLOCK_REALTIME, &timeout) < 0)
    {
        perror("clock_gettime");

        return -1;
    }

    timeout.tv_sec += 2;

    while (!atomic_load(&sw->stopping))
    {
        if (sem_timedwait(&sw->free_ports, &timeout) < 0)
        {
            if (errno == EINTR)
                continue;

            if (errno == ETIMEDOUT && !atomic_load(&sw->stopping))
            {
                static const char full[] = "switch full\n";

                if (socket_write_exact(fd, full, sizeof(full) - 1) < 0 &&
                    errno != EPIPE && errno != ECONNRESET)
                {
                    perror("send");
                }
            }
            else if (!atomic_load(&sw->stopping))
            {
                perror("sem_timedwait");
            }

            return -1;
        }

        pthread_rwlock_wrlock(&sw->ports_lock);

        if (sw->cancel_admission && !atomic_load(&sw->stopping))
        {
            sw->cancel_admission = 0;

            pthread_rwlock_unlock(&sw->ports_lock);

            continue;
        }

        int assigned = 0;

        if (!atomic_load(&sw->stopping))
        {
            for (int i = 0; i < sw->max_ports; i++)
            {
                if (sw->ports[i].enabled && !sw->ports[i].active)
                {
                    if (start_port(sw, fd, i) == 0)
                        assigned = 1;
                    else if (sem_post(&sw->free_ports) < 0)
                        perror("sem_post");

                    break;
                }
            }
        }

        pthread_rwlock_unlock(&sw->ports_lock);

        if (assigned)
            return 0;

        break;
    }

    return -1;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr,
                "Usage: ./switch <tcp_port> <num_ports>\n");

        return 1;
    }

    char *end1;

    char *end2;

    long tcp_port = strtol(argv[1], &end1, 10);

    long num_ports = strtol(argv[2], &end2, 10);

    if (*end1 != '\0' ||
        *end2 != '\0' ||
        tcp_port < 1 ||
        tcp_port > 65535 ||
        num_ports < 1 ||
        num_ports > SWITCH_MAX_PORTS)
    {
        fprintf(stderr, "Invalid arguments.\n");

        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        perror("signal");

        return 1;
    }

    umask(077);

    switch_state_t sw;

    if (init_switch(&sw, (int)tcp_port, (int)num_ports) < 0)
    {
        fprintf(stderr, "Switch initialization failed.\n");

        return 1;
    }

    printf("Virtual switch listening on TCP port %ld with %ld ports\n",
           tcp_port, num_ports);

    fflush(stdout);

    struct pollfd input[2] = {
        {sw.server_fd, POLLIN, 0},
        {sw.wake[0], POLLIN, 0}
    };

    int status = 0;

    while (!atomic_load(&sw.stopping))
    {
        if (poll(input, 2, -1) < 0)
        {
            if (errno == EINTR)
                continue;

            perror("poll");

            status = 1;

            break;
        }

        if (input[1].revents != 0)
            break;

        if (input[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "Listening socket failed.\n");

            status = 1;

            break;
        }

        if (!(input[0].revents & POLLIN))
            continue;

        int fd = accept(sw.server_fd, NULL, NULL);

        if (fd < 0)
        {
            if (errno != EINTR)
                perror("accept");

            continue;
        }

        if (admit_host(&sw, fd) < 0 && close(fd) < 0)
            perror("close");
    }

    destroy_switch(&sw);

    return status;
}
