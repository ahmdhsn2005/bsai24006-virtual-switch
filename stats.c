#include "admin.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int switch_path(char *path, size_t size, const char *name)
{
    const char *directory = getenv("SWITCH_RUNTIME_DIR");

    if (directory == NULL || *directory == '\0')
        directory = ".";

    int length = snprintf(path, size, "%s/%s", directory, name);

    if (length < 0 || (size_t)length >= size)
    {
        errno = ENAMETOOLONG;

        return -1;
    }

    return 0;
}

int admin_address(struct sockaddr_un *address)
{
    memset(address, 0, sizeof(*address));

    address->sun_family = AF_UNIX;

    return switch_path(address->sun_path, sizeof(address->sun_path), "switch.sock");
}

int stats_lock(switch_stats_t *stats)
{
    int result = pthread_mutex_lock(&stats->lock);

    if (result == EOWNERDEAD)
    {
        result = pthread_mutex_consistent(&stats->lock);

        if (result != 0)
            pthread_mutex_unlock(&stats->lock);
    }

    if (result != 0)
    {
        errno = result;

        return -1;
    }

    return 0;
}

int stats_print(switch_stats_t *stats, FILE *output)
{
    if (stats_lock(stats) < 0)
        return -1;

    int running = stats->running;

    int count = stats->max_ports;

    port_stats_t ports[SWITCH_MAX_PORTS];

    if (count < 1 || count > SWITCH_MAX_PORTS)
    {
        pthread_mutex_unlock(&stats->lock);

        errno = EINVAL;

        return -1;
    }

    memcpy(ports, stats->ports, sizeof(ports));

    pthread_mutex_unlock(&stats->lock);

    if (!running)
        return 0;

    for (int i = 0; i < count; i++)
    {
        if (fprintf(output,
                    "Port %d frames_in=%lu frames_out=%lu frames_dropped=%lu frames_mirrored=%lu\n",
                    i, ports[i].frames_in, ports[i].frames_out,
                    ports[i].frames_dropped, ports[i].frames_mirrored) < 0)
        {
            return -1;
        }
    }

    return 1;
}
