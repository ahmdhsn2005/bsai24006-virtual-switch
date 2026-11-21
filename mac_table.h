#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>

#define MAC_BUCKETS 32
#define MAC_TABLE_LIMIT 1024
#define MAC_MAX_PER_PORT 8

typedef struct mac_entry
{
    uint8_t mac[6];

    int port;

    uint16_t vlan;

    time_t last_seen;

    uint64_t last_used;

    struct mac_entry *next;

} mac_entry_t;

typedef struct
{
    mac_entry_t *head;

    pthread_mutex_t lock;

} mac_bucket_t;

typedef struct
{
    mac_bucket_t buckets[MAC_BUCKETS];

    pthread_mutex_t change_lock;

    atomic_uint_fast64_t clock;

    size_t size;

} mac_table_t;

int mac_table_init(mac_table_t *table);

int mac_table_learn(
    mac_table_t *table,
    const uint8_t mac[6],
    int port
);

int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port
);

int mac_table_learn_vlan(
    mac_table_t *table,
    const uint8_t mac[6],
    uint16_t vlan,
    int port,
    int *previous_port
);

int mac_table_lookup_vlan(
    mac_table_t *table,
    const uint8_t mac[6],
    uint16_t vlan,
    int *port
);

void mac_table_age(mac_table_t *table, unsigned int age_seconds);

void mac_table_remove_port(mac_table_t *table, int port);

int mac_table_print(mac_table_t *table, FILE *output);

void mac_table_clear(mac_table_t *table);

void mac_table_destroy(mac_table_t *table);

#endif