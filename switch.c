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

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "SIGINT requires a lock-free flag");

static atomic_int stop_requested = 0;

static void handle_sigint(int signo)
{
    (void)signo;

    stop_requested = 1;
}

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

static void enqueue_to_port(
    switch_port_t *port,
    shared_frame_t *frame
)
{
    shared_frame_retain(frame);

    if (output_queue_push(&port->output, frame) < 0)
    {
        shared_frame_release(frame);

        atomic_fetch_add(&port->frames_dropped, 1);
    }
}

static int forward_frame(
    switch_port_t *source,
    const frame_t *frame
)
{
    switch_state_t *sw = source->sw;

    int destination_port = -1;

    int learned = mac_table_learn(
        &sw->mac_table,
        frame->src,
        source->id
    );

    if (learned == -2)
    {
        fprintf(stderr,
                "Port %d exceeded MAC limit; shutting it down\n",
                source->id);

        return -1;
    }

    if (learned < 0)
        return 0;

    shared_frame_t *shared = shared_frame_create(frame);

    if (shared == NULL)
        return 0;

    pthread_rwlock_rdlock(&sw->ports_lock);

    if (!is_broadcast(frame->dst) &&
        mac_table_lookup(
            &sw->mac_table,
            frame->dst,
            &destination_port
        ) == 0)
    {
        if (destination_port >= 0 &&
            destination_port < sw->max_ports &&
            destination_port != source->id &&
            sw->ports[destination_port].active)
        {
            enqueue_to_port(
                &sw->ports[destination_port],
                shared
            );
        }
    }
    else
    {
        for (int i = 0; i < sw->max_ports; i++)
        {
            if (i != source->id &&
                sw->ports[i].active)
            {
                enqueue_to_port(
                    &sw->ports[i],
                    shared
                );
            }
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

    frame_t frame;

    while (!atomic_load(&sw->stopping))
    {
        int result = frame_read(port->fd, &frame);

        if (result != 1)
            break;

        port->frames_in++;

        if (forward_frame(port, &frame) < 0)
            break;
    }

    shutdown_socket(port->fd);

    output_queue_close(&port->output);

    pthread_join(port->sender_thread, NULL);

    pthread_rwlock_wrlock(&sw->ports_lock);

    mac_table_remove_port(&sw->mac_table, port->id);

    close(port->fd);

    output_queue_destroy(&port->output);

    port->active = 0;

    printf("Port %d disconnected and released\n", port->id);

    if (sem_post(&sw->free_ports) < 0)
        perror("sem_post");

    pthread_rwlock_unlock(&sw->ports_lock);

    return NULL;
}

static void *sender_main(void *argument)
{
    switch_port_t *port = argument;

    for (;;)
    {
        shared_frame_t *shared =
            output_queue_pop(&port->output);

        if (shared == NULL)
            break;

        if (frame_write(
                port->fd,
                &shared->frame
            ) < 0)
        {
            shared_frame_release(shared);

            shutdown_socket(port->fd);

            break;
        }

        port->frames_out++;

        shared_frame_release(shared);
    }

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

        if (!atomic_load(&sw->stopping))
        {
            pthread_cond_timedwait(
                &sw->timer_changed,
                &sw->timer_lock,
                &deadline
            );
        }

        pthread_mutex_unlock(&sw->timer_lock);

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

        if (!atomic_load(&sw->stopping))
        {
            pthread_cond_timedwait(
                &sw->timer_changed,
                &sw->timer_lock,
                &deadline
            );
        }

        pthread_mutex_unlock(&sw->timer_lock);

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

    memset(port, 0, sizeof(*port));

    port->id = id;

    port->fd = fd;

    port->sw = sw;

    atomic_init(&port->frames_dropped, 0);

    if (output_queue_init(
            &port->output,
            PORT_RATE_FRAMES_PER_SECOND
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

    printf("Host connected on switch port %d\n", id);

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

static int init_switch(
    switch_state_t *sw,
    int tcp_port,
    int max_ports
)
{
    memset(sw, 0, sizeof(*sw));

    sw->server_fd = -1;

    sw->max_ports = max_ports;

    atomic_init(&sw->stopping, 0);

    if (pthread_rwlock_init(
            &sw->ports_lock,
            NULL
        ) != 0)
    {
        return -1;
    }

    if (pthread_mutex_init(
            &sw->timer_lock,
            NULL
        ) != 0)
    {
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    if (pthread_cond_init(
            &sw->timer_changed,
            NULL
        ) != 0)
    {
        pthread_mutex_destroy(&sw->timer_lock);
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    if (sem_init(
            &sw->free_ports,
            0,
            (unsigned int)max_ports
        ) < 0)
    {
        pthread_cond_destroy(&sw->timer_changed);
        pthread_mutex_destroy(&sw->timer_lock);
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    if (mac_table_init(&sw->mac_table) < 0)
    {
        sem_destroy(&sw->free_ports);
        pthread_cond_destroy(&sw->timer_changed);
        pthread_mutex_destroy(&sw->timer_lock);
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    for (int i = 0; i < max_ports; i++)
    {
        sw->ports[i].fd = -1;

        sw->ports[i].id = i;
    }

    sw->server_fd = create_server(tcp_port);

    if (sw->server_fd < 0)
    {
        mac_table_destroy(&sw->mac_table);
        sem_destroy(&sw->free_ports);
        pthread_cond_destroy(&sw->timer_changed);
        pthread_mutex_destroy(&sw->timer_lock);
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    if (pthread_create(
            &sw->aging_thread,
            NULL,
            aging_main,
            sw
        ) != 0)
    {
        close(sw->server_fd);
        mac_table_destroy(&sw->mac_table);
        sem_destroy(&sw->free_ports);
        pthread_cond_destroy(&sw->timer_changed);
        pthread_mutex_destroy(&sw->timer_lock);
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    if (pthread_create(
            &sw->refill_thread,
            NULL,
            refill_main,
            sw
        ) != 0)
    {
        atomic_store(&sw->stopping, 1);
        pthread_mutex_lock(&sw->timer_lock);
        pthread_cond_broadcast(&sw->timer_changed);
        pthread_mutex_unlock(&sw->timer_lock);
        pthread_join(sw->aging_thread, NULL);
        close(sw->server_fd);
        mac_table_destroy(&sw->mac_table);
        sem_destroy(&sw->free_ports);
        pthread_cond_destroy(&sw->timer_changed);
        pthread_mutex_destroy(&sw->timer_lock);
        pthread_rwlock_destroy(&sw->ports_lock);
        return -1;
    }

    return 0;
}

static void destroy_switch(switch_state_t *sw)
{
    atomic_store(&sw->stopping, 1);

    pthread_mutex_lock(&sw->timer_lock);

    pthread_cond_broadcast(&sw->timer_changed);

    pthread_mutex_unlock(&sw->timer_lock);

    if (sw->server_fd >= 0)
        close(sw->server_fd);

    pthread_rwlock_wrlock(&sw->ports_lock);

    for (int i = 0; i < sw->max_ports; i++)
    {
        switch_port_t *port = &sw->ports[i];

        if (port->active)
        {
            shutdown_socket(port->fd);

            output_queue_close(&port->output);
        }
    }

    pthread_rwlock_unlock(&sw->ports_lock);

    pthread_join(sw->aging_thread, NULL);

    pthread_join(sw->refill_thread, NULL);

    for (int i = 0; i < sw->max_ports; i++)
    {
        switch_port_t *port = &sw->ports[i];

        if (port->receiver_started)
            pthread_join(port->receiver_thread, NULL);
    }

    printf("\nFinal MAC table:\n");

    mac_table_print(&sw->mac_table);

    mac_table_destroy(&sw->mac_table);

    sem_destroy(&sw->free_ports);

    pthread_cond_destroy(&sw->timer_changed);

    pthread_mutex_destroy(&sw->timer_lock);

    pthread_rwlock_destroy(&sw->ports_lock);
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

    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = handle_sigint;

    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        perror("signal");

        return 1;
    }

    switch_state_t sw;

    if (init_switch(
            &sw,
            (int)tcp_port,
            (int)num_ports
        ) < 0)
    {
        fprintf(stderr, "Switch initialization failed.\n");

        return 1;
    }

    printf(
        "Virtual switch listening on TCP port %ld with %ld ports\n",
        tcp_port,
        num_ports
    );

    while (!stop_requested)
    {
        struct pollfd pfd;

        pfd.fd = sw.server_fd;

        pfd.events = POLLIN;

        pfd.revents = 0;

        int ready = poll(&pfd, 1, 500);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            perror("poll");

            break;
        }

        if (ready == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "Listening socket failed.\n");

            break;
        }

        if (!(pfd.revents & POLLIN) || stop_requested)
            continue;

        struct sockaddr_in client;

        socklen_t length = sizeof(client);

        int client_fd = accept(
            sw.server_fd,
            (struct sockaddr *)&client,
            &length
        );

        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;

            perror("accept");

            continue;
        }

        struct timespec timeout;

        if (clock_gettime(CLOCK_REALTIME, &timeout) < 0)
        {
            perror("clock_gettime");

            close(client_fd);

            break;
        }

        timeout.tv_sec += 2;

        int available;

        do
        {
            available = sem_timedwait(&sw.free_ports, &timeout);
        }
        while (available < 0 && errno == EINTR && !stop_requested);

        if (available < 0)
        {
            if (errno == ETIMEDOUT && !stop_requested)
            {
                static const char full[] = "switch full\n";

                if (socket_write_exact(
                        client_fd,
                        full,
                        sizeof(full) - 1
                    ) < 0 && errno != EPIPE && errno != ECONNRESET)
                {
                    perror("send");
                }
            }
            else if (errno != EINTR && !stop_requested)
            {
                perror("sem_timedwait");
            }

            close(client_fd);

            continue;
        }

        if (stop_requested)
        {
            close(client_fd);

            break;
        }

        int assigned = -1;

        pthread_rwlock_wrlock(&sw.ports_lock);

        for (int i = 0; i < sw.max_ports; i++)
        {
            if (!sw.ports[i].active)
            {
                assigned = i;

                break;
            }
        }

        if (assigned >= 0)
        {
            if (start_port(
                    &sw,
                    client_fd,
                    assigned
                ) < 0)
            {
                assigned = -1;
            }
        }

        pthread_rwlock_unlock(&sw.ports_lock);

        if (assigned < 0)
        {
            close(client_fd);

            if (sem_post(&sw.free_ports) < 0)
                perror("sem_post");
        }
    }

    destroy_switch(&sw);

    return 0;
}