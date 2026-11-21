#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>

static int socket_fd;

static uint8_t own_mac[6];

static void *receive_main(void *unused)
{
    (void)unused;

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

    return NULL;
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

    signal(SIGPIPE, SIG_IGN);

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

    pthread_t receiver;

    if (pthread_create(
            &receiver,
            NULL,
            receive_main,
            NULL
        ) != 0)
    {
        fprintf(stderr, "Could not create receiver thread.\n");

        close(socket_fd);

        return 1;
    }

    char line[1600];

    while (1)
    {
        printf("> ");

        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL)
            break;

        char *destination = strtok(
            line,
            " \t\r\n"
        );

        if (destination == NULL)
            continue;

        char *message = strtok(NULL, "\r\n");

        if (message == NULL)
            message = "";

        frame_t frame;

        memset(&frame, 0, sizeof(frame));

        if (parse_mac(destination, frame.dst) < 0)
        {
            printf("Invalid destination MAC.\n");

            continue;
        }

        memcpy(frame.src, own_mac, 6);

        frame.vlan = 1;

        frame.priority = 1;

        size_t length = strlen(message);

        if (length > FRAME_MAX_PAYLOAD)
        {
            printf("Message too long.\n");

            continue;
        }

        frame.length = (uint16_t)length;

        memcpy(frame.payload, message, length);

        if (frame_write(socket_fd, &frame) < 0)
        {
            printf("Send failed.\n");

            break;
        }
    }

    shutdown(socket_fd, SHUT_RDWR);

    close(socket_fd);

    pthread_join(receiver, NULL);

    return 0;
}