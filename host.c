#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>

static int socket_fd;

static uint8_t own_mac[6];

static void *receive_main(void *argument)
{
    int done = *(int *)argument;

    frame_t frame;

    while (1)
    {
        int result = frame_read(socket_fd, &frame);

        if (result != 1)
            break;

        printf("\nReceived from ");

        print_mac(frame.src);

        printf(": %.*s\n",
               (int)frame.length,
               frame.payload);

        printf("> ");

        fflush(stdout);
    }

    if (shutdown(socket_fd, SHUT_RDWR) < 0 && errno != ENOTCONN)
        perror("shutdown");

    ssize_t notified;

    do
    {
        notified = write(done, "", 1);
    }
    while (notified < 0 && errno == EINTR);

    if (notified < 0)
        perror("write");

    return NULL;
}

static int send_line(char *line)
{
    char *destination = strtok(
        line,
        " \t\r\n"
    );

    if (destination == NULL)
        return 0;

    char *message = strtok(NULL, "\r\n");

    if (message == NULL)
        message = "";

    frame_t frame;

    memset(&frame, 0, sizeof(frame));

    if (parse_mac(destination, frame.dst) < 0)
    {
        printf("Invalid destination MAC.\n");

        return 0;
    }

    memcpy(frame.src, own_mac, 6);

    frame.vlan = 1;

    frame.priority = 1;

    size_t length = strlen(message);

    if (length > FRAME_MAX_PAYLOAD)
    {
        printf("Message too long.\n");

        return 0;
    }

    frame.length = (uint16_t)length;

    memcpy(frame.payload, message, length);

    if (frame_write(socket_fd, &frame) < 0)
    {
        printf("Send failed.\n");

        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr,
                "Usage: ./host <switch_ip> <tcp_port> <mac>\n");

        return 1;
    }

    char *end;

    long port = strtol(argv[2], &end, 10);

    if (*end != '\0' ||
        port < 1 ||
        port > 65535 ||
        parse_mac(argv[3], own_mac) < 0)
    {
        fprintf(stderr, "Invalid port or MAC address.\n");

        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        perror("signal");

        return 1;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0)
    {
        perror("socket");

        return 1;
    }

    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;

    address.sin_port = htons((uint16_t)port);

    if (inet_pton(
            AF_INET,
            argv[1],
            &address.sin_addr
        ) != 1)
    {
        fprintf(stderr, "Invalid IP address.\n");

        close(socket_fd);

        return 1;
    }

    if (connect(
            socket_fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0)
    {
        perror("connect");

        close(socket_fd);

        return 1;
    }

    printf("Connected to switch.\n");

    printf("Enter: DESTINATION_MAC message\n");

    printf("Example: BB:BB:BB:BB:BB:02 Hello\n");

    int done[2];

    if (pipe(done) < 0)
    {
        perror("pipe");

        close(socket_fd);

        return 1;
    }

    pthread_t receiver;

    if (pthread_create(
            &receiver,
            NULL,
            receive_main,
            &done[1]
        ) != 0)
    {
        fprintf(stderr, "Could not create receiver thread.\n");

        close(done[0]);

        close(done[1]);

        close(socket_fd);

        return 1;
    }

    struct pollfd input[2] = {
        {STDIN_FILENO, POLLIN, 0},
        {done[0], POLLIN, 0}
    };

    char line[1600];

    size_t used = 0;

    int too_long = 0;

    int result = 0;

    printf("> ");

    fflush(stdout);

    while (result == 0)
    {
        if (poll(input, 2, -1) < 0)
        {
            if (errno == EINTR)
                continue;

            perror("poll");

            result = -1;

            break;
        }

        if (input[1].revents != 0)
        {
            printf("\nDisconnected from switch.\n");

            break;
        }

        char buffer[sizeof(line)];

        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            perror("read");

            result = -1;

            break;
        }

        if (n == 0)
        {
            if (too_long)
                printf("Message too long.\n");
            else if (used > 0)
            {
                line[used] = '\0';

                result = send_line(line);
            }

            break;
        }

        for (ssize_t i = 0; i < n; i++)
        {
            if (buffer[i] == '\n')
            {
                if (too_long)
                    printf("Message too long.\n");
                else
                {
                    line[used] = '\0';

                    result = send_line(line);

                    if (result < 0)
                        break;
                }

                used = 0;

                too_long = 0;

                printf("> ");

                fflush(stdout);
            }
            else if (used < sizeof(line) - 1)
            {
                line[used++] = buffer[i];
            }
            else
            {
                too_long = 1;
            }
        }
    }

    if (shutdown(socket_fd, SHUT_RDWR) < 0 && errno != ENOTCONN)
        perror("shutdown");

    pthread_join(receiver, NULL);

    close(socket_fd);

    close(done[0]);

    close(done[1]);

    return result < 0 ? 1 : 0;
}