#include "frame.h"
#include "mac_table.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void)
{
    int pair[2];
    uint8_t parsed_mac[6];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0)
    {
        perror("socketpair");

        return 1;
    }

    frame_t original;

    frame_t received;

    memset(&original, 0, sizeof(original));

    original.length = 12;

    original.vlan = 10;

    original.priority = 1;

    uint8_t source[6] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01
    };

    uint8_t destination[6] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66
    };

    memcpy(original.src, source, 6);

    memcpy(original.dst, destination, 6);

    memcpy(original.payload, "Hello World!", 12);

    if (frame_write(pair[0], &original) < 0 ||
        frame_read(pair[1], &received) != 1)
    {
        fprintf(stderr, "Frame socket test failed.\n");

        close(pair[0]);

        close(pair[1]);

        return 1;
    }

    if (original.length != received.length ||
        original.vlan != received.vlan ||
        original.priority != received.priority ||
        memcmp(original.src, received.src, 6) != 0 ||
        memcmp(original.dst, received.dst, 6) != 0 ||
        memcmp(
            original.payload,
            received.payload,
            original.length
        ) != 0)
    {
        fprintf(stderr, "Frame comparison failed.\n");

        close(pair[0]);

        close(pair[1]);

        return 1;
    }

    close(pair[0]);

    close(pair[1]);

    if (parse_mac("01:23:45:67:89:AB", parsed_mac) < 0 ||
        parsed_mac[0] != 0x01 ||
        parsed_mac[5] != 0xAB ||
        parse_mac("01:23:45:67:89:AB:CD", parsed_mac) == 0)
    {
        fprintf(stderr, "MAC address parsing test failed.\n");

        return 1;
    }

    mac_table_t table;

    if (mac_table_init(&table) < 0)
    {
        fprintf(stderr, "MAC table initialization failed.\n");

        return 1;
    }

    if (mac_table_learn(&table, source, 2) != 0)
    {
        fprintf(stderr, "MAC learning failed.\n");

        mac_table_destroy(&table);

        return 1;
    }

    int found_port = -1;

    if (mac_table_lookup(
            &table,
            source,
            &found_port
        ) != 0 ||
        found_port != 2)
    {
        fprintf(stderr, "MAC lookup failed.\n");

        mac_table_destroy(&table);

        return 1;
    }

    mac_table_destroy(&table);

    printf("Frame and MAC table tests PASSED.\n");

    return 0;
}