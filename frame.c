#include "frame.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static int read_exact(int fd, void *buffer, size_t length)
{
    size_t done = 0;
    uint8_t *bytes = buffer;

    while (done < length)
    {
        ssize_t n = recv(fd, bytes + done, length - done, 0);

        if (n == 0)
            return done == 0 ? 0 : -1;

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            return -1;
        }

        done += (size_t)n;
    }

    return 1;
}

int socket_write_exact(int fd, const void *buffer, size_t length)
{
    size_t done = 0;
    const uint8_t *bytes = buffer;

    while (done < length)
    {
        ssize_t n = send(fd, bytes + done,
                         length - done, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
        {
            errno = EPIPE;

            return -1;
        }

        done += (size_t)n;
    }

    return 0;
}

void serialize_frame(const frame_t *frame, uint8_t *buffer)
{
    uint16_t value = htons(frame->length);

    memcpy(buffer, &value, 2);
    memcpy(buffer + 2, frame->src, 6);
    memcpy(buffer + 8, frame->dst, 6);

    value = htons(frame->vlan);

    memcpy(buffer + 14, &value, 2);

    buffer[16] = frame->priority;
}

int deserialize_frame(const uint8_t *buffer, frame_t *frame)
{
    uint16_t value;

    memcpy(&value, buffer, 2);
    frame->length = ntohs(value);

    memcpy(frame->src, buffer + 2, 6);
    memcpy(frame->dst, buffer + 8, 6);

    memcpy(&value, buffer + 14, 2);
    frame->vlan = ntohs(value);

    frame->priority = buffer[16];

    if (frame->length > FRAME_MAX_PAYLOAD ||
        frame->vlan < 1 ||
        frame->vlan > 4094 ||
        frame->priority > 2)
    {
        return -1;
    }

    return 0;
}

int frame_read(int fd, frame_t *frame)
{
    uint8_t header[FRAME_HEADER_SIZE];

    int result = read_exact(fd, header, sizeof(header));

    if (result <= 0)
        return result;

    if (deserialize_frame(header, frame) < 0)
        return -1;

    if (frame->length > 0)
    {
        result = read_exact(fd, frame->payload, frame->length);

        if (result != 1)
            return -1;
    }

    return 1;
}

int frame_write(int fd, const frame_t *frame)
{
    uint8_t header[FRAME_HEADER_SIZE];

    if (frame->length > FRAME_MAX_PAYLOAD ||
        frame->vlan < 1 ||
        frame->vlan > 4094 ||
        frame->priority > 2)
    {
        return -1;
    }

    serialize_frame(frame, header);

    if (socket_write_exact(fd, header, sizeof(header)) < 0)
        return -1;

    if (frame->length > 0)
    {
        if (socket_write_exact(fd, frame->payload, frame->length) < 0)
            return -1;
    }

    return 0;
}

int parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int b[6];
    char extra;

    if (sscanf(text, "%x:%x:%x:%x:%x:%x%c",
               &b[0], &b[1], &b[2],
               &b[3], &b[4], &b[5], &extra) != 6)
    {
        return -1;
    }

    for (int i = 0; i < 6; i++)
    {
        if (b[i] > 255)
            return -1;

        mac[i] = (uint8_t)b[i];
    }

    return 0;
}

void print_mac(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++)
    {
        printf("%02X", mac[i]);

        if (i != 5)
            printf(":");
    }
}