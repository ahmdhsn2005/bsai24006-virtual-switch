#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_HEADER_SIZE 17
#define FRAME_MAX_PAYLOAD 1500

typedef struct
{
    uint16_t length;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t vlan;
    uint8_t priority;
    uint8_t payload[FRAME_MAX_PAYLOAD];
} frame_t;

void serialize_frame(const frame_t *frame, uint8_t *buffer);
int deserialize_frame(const uint8_t *buffer, frame_t *frame);

int frame_read(int fd, frame_t *frame);
int frame_write(int fd, const frame_t *frame);
int socket_write_exact(int fd, const void *buffer, size_t length);

int parse_mac(const char *text, uint8_t mac[6]);
void print_mac(const uint8_t mac[6]);

#endif