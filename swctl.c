#include "admin.h"
#include "frame.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int watch_stats(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);

    if (fd < 0)
    {
        perror("shm_open");

        return 1;
    }

    struct stat info;

    if (fstat(fd, &info) < 0 || info.st_size != (off_t)sizeof(switch_stats_t))
    {
        fprintf(stderr, "Invalid statistics file.\n");

        close(fd);

        return 1;
    }

    switch_stats_t *stats = mmap(NULL, sizeof(*stats), PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);

    close(fd);

    if (stats == MAP_FAILED)
    {
        perror("mmap");

        return 1;
    }

    int status = 0;

    for (;;)
    {
        int result = stats_print(stats, stdout);

        if (result == 0)
            break;

        if (result < 0 || fflush(stdout) == EOF)
        {
            status = 1;

            break;
        }

        if (poll(NULL, 0, 1000) < 0)
        {
            perror("poll");

            status = 1;

            break;
        }
    }

    if (munmap(stats, sizeof(*stats)) < 0)
    {
        perror("munmap");

        status = 1;
    }

    return status;
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 4)
    {
        fprintf(stderr, "Usage: ./swctl <command> [arguments]\n");

        return 1;
    }

    int watch = argc == 3 && strcmp(argv[1], "stats") == 0 &&
                strcmp(argv[2], "--watch") == 0;

    char command[ADMIN_COMMAND_SIZE];

    size_t used = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strpbrk(argv[i], "\r\n") != NULL)
        {
            fprintf(stderr, "Invalid command.\n");

            return 1;
        }

        int added = snprintf(command + used, sizeof(command) - used, "%s%s",
                             i == 1 ? "" : " ", argv[i]);

        if (added < 0 || (size_t)added >= sizeof(command) - used - 1)
        {
            fprintf(stderr, "Command too long.\n");

            return 1;
        }

        used += (size_t)added;
    }

    command[used++] = '\n';

    struct sockaddr_un address;

    if (admin_address(&address) < 0)
    {
        perror("admin socket path");

        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");

        return 1;
    }

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        socket_write_exact(fd, command, used) < 0)
    {
        perror("admin socket");

        close(fd);

        return 1;
    }

    FILE *input = fdopen(fd, "r");

    if (input == NULL)
    {
        perror("fdopen");

        close(fd);

        return 1;
    }

    int status = 0;

    char name[STATS_NAME_SIZE] = "";

    char line[256];

    while (fgets(line, sizeof(line), input) != NULL)
    {
        if (watch && strncmp(line, "SHM ", 4) == 0)
        {
            size_t length = strcspn(line + 4, "\n");

            if (length > 0 && length < sizeof(name) && line[4] == '/')
            {
                memcpy(name, line + 4, length);

                name[length] = '\0';
            }

            continue;
        }

        FILE *output = stdout;

        if (strncmp(line, "error:", 6) == 0)
        {
            output = stderr;

            status = 1;
        }

        if (fputs(line, output) == EOF)
        {
            status = 1;

            break;
        }
    }

    if (ferror(input))
        status = 1;

    if (fclose(input) == EOF)
        status = 1;

    if (watch && status == 0)
    {
        if (*name == '\0')
        {
            fprintf(stderr, "Statistics are unavailable.\n");

            return 1;
        }

        return watch_stats(name);
    }

    return status;
}
