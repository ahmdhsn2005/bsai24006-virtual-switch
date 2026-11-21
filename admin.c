#include "switch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_PORT_RATE 1000000

static int parse_number(const char *text, int minimum, int maximum)
{
    if (text == NULL || *text == '\0')
        return -1;

    char *end;

    errno = 0;

    long number = strtol(text, &end, 10);

    if (errno != 0 || *end != '\0' || number < minimum || number > maximum)
        return -1;

    return (int)number;
}

static int split_words(char *line, char *words[4])
{
    int count = 0;

    char *next;

    char *word = strtok_r(line, " \t\r\n", &next);

    while (word != NULL && count < 4)
    {
        words[count++] = word;

        word = strtok_r(NULL, " \t\r\n", &next);
    }

    return count;
}

static int parse_config_line(
    port_config_t *config,
    int max_ports,
    char *line
)
{
    char *comment = strchr(line, '#');

    if (comment != NULL)
        *comment = '\0';

    char *words[4];

    int count = split_words(line, words);

    if (count == 0)
        return 0;

    if (count < 2 || count > 3)
        return -1;

    int port = parse_number(words[1], 0, max_ports - 1);

    if (port < 0)
        return -1;

    if (count == 2 && strcmp(words[0], "trunk") == 0)
    {
        config[port].vlan = 0;

        return 0;
    }

    if (count != 3)
        return -1;

    if (strcmp(words[0], "mirror") == 0 && strcmp(words[2], "off") == 0)
    {
        config[port].mirror = -1;

        return 0;
    }

    if (strcmp(words[0], "mirror") == 0)
    {
        int mirror = parse_number(words[2], 0, max_ports - 1);

        if (mirror < 0 || mirror == port)
            return -1;

        config[port].mirror = mirror;
    }
    else if (strcmp(words[0], "vlan") == 0)
    {
        int vlan = parse_number(words[2], 1, 4094);

        if (vlan < 0)
            return -1;

        config[port].vlan = vlan;
    }
    else if (strcmp(words[0], "rate") == 0)
    {
        int rate = parse_number(words[2], 1, MAX_PORT_RATE);

        if (rate < 0)
            return -1;

        config[port].rate = (unsigned int)rate;
    }
    else
    {
        return -1;
    }

    return 0;
}

int switch_reload_config(switch_state_t *sw, int initial)
{
    port_config_t config[SWITCH_MAX_PORTS];

    for (int i = 0; i < sw->max_ports; i++)
    {
        config[i].vlan = 1;

        config[i].mirror = -1;

        config[i].rate = PORT_RATE_FRAMES_PER_SECOND;
    }

    char default_path[1024];

    const char *path = getenv("SWITCH_CONFIG");

    int use_default = path == NULL || *path == '\0';

    if (use_default)
    {
        if (switch_path(default_path, sizeof(default_path), "switch.conf") < 0)
            return -1;

        path = default_path;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);

    if (fd < 0)
    {
        if (initial && use_default && errno == ENOENT)
        {
            switch_apply_config(sw, config);

            return 0;
        }

        logger_write(&sw->log, "Could not open config %s: %s", path, strerror(errno));

        return -1;
    }

    struct stat info;

    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode))
    {
        logger_write(&sw->log, "Could not read config %s as a regular file", path);

        close(fd);

        return -1;
    }

    FILE *file = fdopen(fd, "r");

    if (file == NULL)
    {
        logger_write(&sw->log, "Could not read config %s: %s", path, strerror(errno));

        close(fd);

        return -1;
    }

    char line[ADMIN_COMMAND_SIZE];

    unsigned int line_number = 0;

    int result = 0;

    while (fgets(line, sizeof(line), file) != NULL)
    {
        line_number++;

        if ((strchr(line, '\n') == NULL && !feof(file)) ||
            parse_config_line(config, sw->max_ports, line) < 0)
        {
            result = -1;

            break;
        }
    }

    if (ferror(file))
        result = -1;

    if (fclose(file) == EOF)
        result = -1;

    int trunks = 0;

    for (int i = 0; i < sw->max_ports; i++)
    {
        if (config[i].vlan == 0)
            trunks++;
    }

    if (result < 0 || trunks > 1)
    {
        logger_write(&sw->log, "Invalid config %s near line %u", path, line_number);

        return -1;
    }

    switch_apply_config(sw, config);

    logger_write(&sw->log, "Configuration loaded from %s", path);

    return 0;
}

static int show_ports(switch_state_t *sw, FILE *output)
{
    pthread_rwlock_rdlock(&sw->ports_lock);

    int result = 0;

    for (int i = 0; i < sw->max_ports && result == 0; i++)
    {
        switch_port_t *port = &sw->ports[i];

        port_config_t config = sw->config[i];

        char vlan[16] = "trunk";

        if (config.vlan != 0 &&
            snprintf(vlan, sizeof(vlan), "%d", config.vlan) < 0)
        {
            result = -1;

            break;
        }

        const char *state = "down";

        if (port->enabled)
            state = port->active ? "connected" : "available";

        if (fprintf(output, "Port %d %s vlan=%s mirror=%d rate=%u\n",
                    i, state, vlan, config.mirror, config.rate) < 0)
        {
            result = -1;
        }
    }

    pthread_rwlock_unlock(&sw->ports_lock);

    return result;
}

static int handle_command(switch_state_t *sw, char *line, FILE *output)
{
    char *words[4];

    int count = split_words(line, words);

    if (count == 1 && strcmp(words[0], "stats") == 0)
        return stats_print(sw->admin.stats, output) < 0 ? -1 : 0;

    if (count == 2 && strcmp(words[0], "stats") == 0 &&
        strcmp(words[1], "--watch") == 0)
    {
        return fprintf(output, "SHM %s\n", sw->admin.stats_name) < 0 ? -1 : 0;
    }

    if (count == 2 && strcmp(words[0], "show") == 0)
    {
        if (strcmp(words[1], "mac") == 0)
            return mac_table_print(&sw->mac_table, output);

        if (strcmp(words[1], "ports") == 0)
            return show_ports(sw, output);
    }

    if (count == 2 && strcmp(words[0], "clear") == 0 &&
        strcmp(words[1], "mac") == 0)
    {
        pthread_rwlock_wrlock(&sw->ports_lock);

        mac_table_clear(&sw->mac_table);

        pthread_rwlock_unlock(&sw->ports_lock);

        return fputs("OK\n", output) == EOF ? -1 : 0;
    }

    int port = count >= 2 ? parse_number(words[1], 0, sw->max_ports - 1) : -1;

    if (port >= 0 && count == 2 && strcmp(words[0], "shutdown") == 0)
    {
        if (switch_shutdown_port(sw, port) == 0)
            return fputs("OK\n", output) == EOF ? -1 : 0;

        return fputs("error: could not shut down port\n", output) == EOF ? -1 : 0;
    }

    if (port >= 0 && count == 2 && strcmp(words[0], "up") == 0)
    {
        if (switch_up_port(sw, port) == 0)
            return fputs("OK\n", output) == EOF ? -1 : 0;

        return fputs("error: could not enable port\n", output) == EOF ? -1 : 0;
    }

    if (port >= 0 && count == 2 && strcmp(words[0], "trunk") == 0)
    {
        switch_set_vlan(sw, port, 0);

        return fputs("OK\n", output) == EOF ? -1 : 0;
    }

    if (port >= 0 && count == 3 && strcmp(words[0], "vlan") == 0)
    {
        int vlan = parse_number(words[2], 1, 4094);

        if (vlan > 0)
        {
            switch_set_vlan(sw, port, vlan);

            return fputs("OK\n", output) == EOF ? -1 : 0;
        }
    }

    if (port >= 0 && count == 3 && strcmp(words[0], "mirror") == 0)
    {
        int mirror = parse_number(words[2], 0, sw->max_ports - 1);

        if ((mirror >= 0 && mirror != port) || strcmp(words[2], "off") == 0)
        {
            pthread_rwlock_wrlock(&sw->ports_lock);

            sw->config[port].mirror = mirror;

            pthread_rwlock_unlock(&sw->ports_lock);

            return fputs("OK\n", output) == EOF ? -1 : 0;
        }
    }

    return fputs("error: invalid command or arguments\n", output) == EOF ? -1 : 0;
}

static int read_command(switch_state_t *sw, int fd, char *line)
{
    struct timespec started;

    if (clock_gettime(CLOCK_MONOTONIC, &started) < 0)
        return -1;

    struct pollfd input[2] = {
        {fd, POLLIN, 0},
        {sw->wake[0], POLLIN, 0}
    };

    size_t used = 0;

    while (used < ADMIN_COMMAND_SIZE - 1)
    {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            return -1;

        long elapsed = (now.tv_sec - started.tv_sec) * 1000 +
                       (now.tv_nsec - started.tv_nsec) / 1000000;

        if (elapsed >= 1000)
            return -1;

        int ready = poll(input, 2, (int)(1000 - elapsed));

        if (ready < 0 && errno == EINTR)
            continue;

        if (ready <= 0 || input[1].revents != 0)
            return -1;

        ssize_t n = recv(fd, line + used, ADMIN_COMMAND_SIZE - 1 - used, MSG_DONTWAIT);

        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;

        if (n <= 0)
            return -1;

        for (ssize_t i = 0; i < n; i++)
        {
            if (line[used + i] == '\0')
                return -1;

            if (line[used + i] == '\n')
            {
                line[used + i] = '\0';

                return 0;
            }
        }

        used += (size_t)n;
    }

    return -1;
}

static void service_client(switch_state_t *sw, int fd)
{
    struct timeval timeout = {1, 0};

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        return;

    char line[ADMIN_COMMAND_SIZE];

    if (read_command(sw, fd, line) < 0)
        return;

    char *data = NULL;

    size_t length = 0;

    FILE *output = open_memstream(&data, &length);

    if (output == NULL)
        return;

    int result = handle_command(sw, line, output);

    if (fclose(output) == EOF)
        result = -1;

    if (result < 0)
    {
        static const char error[] = "error: command failed\n";

        socket_write_exact(fd, error, sizeof(error) - 1);
    }
    else if (socket_write_exact(fd, data, length) < 0 &&
             errno != EPIPE && errno != ECONNRESET && errno != EAGAIN)
    {
        logger_write(&sw->log, "Admin reply failed: %s", strerror(errno));
    }

    free(data);
}

static void *admin_main(void *argument)
{
    switch_state_t *sw = argument;

    struct pollfd input[2] = {
        {sw->admin.fd, POLLIN, 0},
        {sw->wake[0], POLLIN, 0}
    };

    while (!atomic_load(&sw->stopping))
    {
        int ready = poll(input, 2, -1);

        if (ready < 0 && errno == EINTR)
            continue;

        if (ready < 0 || input[1].revents != 0)
            break;

        if (!(input[0].revents & POLLIN))
            break;

        int fd = accept(sw->admin.fd, NULL, NULL);

        if (fd < 0)
        {
            if (errno != EINTR)
                logger_write(&sw->log, "Admin accept failed: %s", strerror(errno));

            continue;
        }

        service_client(sw, fd);

        if (close(fd) < 0)
            logger_write(&sw->log, "Admin close failed: %s", strerror(errno));
    }

    return NULL;
}

static int start_stats(switch_state_t *sw)
{
    admin_t *admin = &sw->admin;

    int length = snprintf(admin->stats_name, sizeof(admin->stats_name),
                          "/virtual_switch_%lu_%ld",
                          (unsigned long)getuid(), (long)getpid());

    if (length < 0 || (size_t)length >= sizeof(admin->stats_name))
        return -1;

    int fd = shm_open(admin->stats_name, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (fd < 0)
        return -1;

    if (ftruncate(fd, sizeof(switch_stats_t)) < 0)
    {
        close(fd);

        shm_unlink(admin->stats_name);

        return -1;
    }

    void *mapping = mmap(NULL, sizeof(switch_stats_t), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);

    close(fd);

    if (mapping == MAP_FAILED)
    {
        shm_unlink(admin->stats_name);

        return -1;
    }

    admin->stats = mapping;

    pthread_mutexattr_t attributes;

    int result = pthread_mutexattr_init(&attributes);

    if (result == 0)
    {
        result = pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED);

        if (result == 0)
            result = pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);

        if (result == 0)
            result = pthread_mutex_init(&admin->stats->lock, &attributes);

        pthread_mutexattr_destroy(&attributes);
    }

    if (result != 0)
    {
        munmap(mapping, sizeof(switch_stats_t));

        shm_unlink(admin->stats_name);

        return -1;
    }

    admin->stats->running = 1;

    admin->stats->max_ports = sw->max_ports;

    return 0;
}

int admin_start(switch_state_t *sw)
{
    admin_t *admin = &sw->admin;

    struct sockaddr_un address;

    if (admin_address(&address) < 0)
        return -1;

    admin->fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (admin->fd < 0)
        return -1;

    if (bind(admin->fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        close(admin->fd);

        return -1;
    }

    memcpy(admin->path, address.sun_path, sizeof(admin->path));

    if (start_stats(sw) < 0)
    {
        unlink(admin->path);

        close(admin->fd);

        return -1;
    }

    if (listen(admin->fd, SWITCH_MAX_PORTS) < 0 ||
        pthread_create(&admin->thread, NULL, admin_main, sw) != 0)
    {
        admin_stop(sw);

        return -1;
    }

    return 0;
}

void admin_stop(switch_state_t *sw)
{
    admin_t *admin = &sw->admin;

    if (stats_lock(admin->stats) == 0)
    {
        admin->stats->running = 0;

        pthread_mutex_unlock(&admin->stats->lock);
    }

    if (shm_unlink(admin->stats_name) < 0)
        logger_write(&sw->log, "Could not remove statistics: %s", strerror(errno));

    if (munmap(admin->stats, sizeof(*admin->stats)) < 0)
        logger_write(&sw->log, "Could not unmap statistics: %s", strerror(errno));

    if (close(admin->fd) < 0)
        logger_write(&sw->log, "Could not close admin socket: %s", strerror(errno));

    if (unlink(admin->path) < 0)
        logger_write(&sw->log, "Could not remove admin socket: %s", strerror(errno));
}
