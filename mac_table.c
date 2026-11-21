#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int hash_mac(const uint8_t mac[6])
{
    unsigned int hash = 0;

    for (int i = 0; i < 6; i++)
        hash = hash * 31U + mac[i];

    return hash % MAC_BUCKETS;
}

int mac_table_init(mac_table_t *table)
{
    table->size = 0;

    atomic_init(&table->clock, 1);

    if (pthread_mutex_init(&table->change_lock, NULL) != 0)
        return -1;

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        table->buckets[i].head = NULL;

        if (pthread_mutex_init(&table->buckets[i].lock, NULL) != 0)
        {
            while (i > 0)
            {
                i--;
                pthread_mutex_destroy(&table->buckets[i].lock);
            }

            pthread_mutex_destroy(&table->change_lock);
            return -1;
        }
    }

    return 0;
}

static size_t count_on_port(mac_table_t *table, int port)
{
    size_t count = 0;

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t *e = table->buckets[i].head;

        while (e != NULL)
        {
            if (e->port == port)
                count++;

            e = e->next;
        }

        pthread_mutex_unlock(&table->buckets[i].lock);
    }

    return count;
}

static void evict_lru(mac_table_t *table)
{
    mac_entry_t *oldest = NULL;

    int oldest_bucket = -1;

    uint64_t oldest_value = UINT64_MAX;

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t *e = table->buckets[i].head;

        while (e != NULL)
        {
            uint64_t used = atomic_load(&e->last_used);

            if (used < oldest_value)
            {
                oldest_value = used;
                oldest = e;
                oldest_bucket = i;
            }

            e = e->next;
        }

        pthread_mutex_unlock(&table->buckets[i].lock);
    }

    if (oldest_bucket >= 0)
    {
        pthread_mutex_lock(&table->buckets[oldest_bucket].lock);

        mac_entry_t **link = &table->buckets[oldest_bucket].head;

        while (*link != NULL && *link != oldest)
            link = &(*link)->next;

        if (*link == oldest)
        {
            *link = oldest->next;

            free(oldest);

            table->size--;
        }

        pthread_mutex_unlock(&table->buckets[oldest_bucket].lock);
    }
}

int mac_table_learn(mac_table_t *table, const uint8_t mac[6], int port)
{
    unsigned int index = hash_mac(mac);

    pthread_mutex_lock(&table->change_lock);

    pthread_mutex_lock(&table->buckets[index].lock);

    mac_entry_t *e = table->buckets[index].head;

    while (e != NULL)
    {
        if (memcmp(e->mac, mac, 6) == 0)
        {
            if (e->port != port)
            {
                printf("MAC moved: port %d -> port %d\n",
                       e->port, port);
            }

            e->port = port;

            atomic_store(&e->last_seen, (int64_t)time(NULL));

            atomic_store(
                &e->last_used,
                atomic_fetch_add(&table->clock, 1)
            );

            pthread_mutex_unlock(&table->buckets[index].lock);

            pthread_mutex_unlock(&table->change_lock);

            return 0;
        }

        e = e->next;
    }

    pthread_mutex_unlock(&table->buckets[index].lock);

    if (count_on_port(table, port) >= MAC_MAX_PER_PORT)
    {
        pthread_mutex_unlock(&table->change_lock);

        return -2;
    }

    if (table->size >= MAC_TABLE_LIMIT)
        evict_lru(table);

    mac_entry_t *entry = calloc(1, sizeof(*entry));

    if (entry == NULL)
    {
        pthread_mutex_unlock(&table->change_lock);

        return -1;
    }

    memcpy(entry->mac, mac, 6);

    entry->port = port;

    atomic_init(&entry->last_seen, (int64_t)time(NULL));

    atomic_init(
        &entry->last_used,
        atomic_fetch_add(&table->clock, 1)
    );

    pthread_mutex_lock(&table->buckets[index].lock);

    entry->next = table->buckets[index].head;

    table->buckets[index].head = entry;

    table->size++;

    pthread_mutex_unlock(&table->buckets[index].lock);

    pthread_mutex_unlock(&table->change_lock);

    return 0;
}

int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port
)
{
    unsigned int index = hash_mac(mac);

    int found = -1;

    pthread_mutex_lock(&table->buckets[index].lock);

    mac_entry_t *e = table->buckets[index].head;

    while (e != NULL)
    {
        if (memcmp(e->mac, mac, 6) == 0)
        {
            *port = e->port;

            atomic_store(&e->last_seen, (int64_t)time(NULL));

            atomic_store(
                &e->last_used,
                atomic_fetch_add(&table->clock, 1)
            );

            found = 0;

            break;
        }

        e = e->next;
    }

    pthread_mutex_unlock(&table->buckets[index].lock);

    return found;
}

void mac_table_age(mac_table_t *table, unsigned int age_seconds)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&table->change_lock);

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t **link = &table->buckets[i].head;

        while (*link != NULL)
        {
            mac_entry_t *e = *link;

            time_t last_seen = (time_t)atomic_load(&e->last_seen);

            if (now - last_seen >= (time_t)age_seconds)
            {
                *link = e->next;

                free(e);

                table->size--;
            }
            else
            {
                link = &e->next;
            }
        }

        pthread_mutex_unlock(&table->buckets[i].lock);
    }

    pthread_mutex_unlock(&table->change_lock);
}

void mac_table_print(mac_table_t *table)
{
    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t *e = table->buckets[i].head;

        while (e != NULL)
        {
            printf("MAC ");

            for (int j = 0; j < 6; j++)
            {
                printf("%02X", e->mac[j]);

                if (j != 5)
                    printf(":");
            }

            printf(" -> port %d\n", e->port);

            e = e->next;
        }

        pthread_mutex_unlock(&table->buckets[i].lock);
    }
}

void mac_table_clear(mac_table_t *table)
{
    pthread_mutex_lock(&table->change_lock);

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t *e = table->buckets[i].head;

        while (e != NULL)
        {
            mac_entry_t *next = e->next;

            free(e);

            e = next;
        }

        table->buckets[i].head = NULL;

        pthread_mutex_unlock(&table->buckets[i].lock);
    }

    table->size = 0;

    pthread_mutex_unlock(&table->change_lock);
}

void mac_table_destroy(mac_table_t *table)
{
    mac_table_clear(table);

    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_destroy(&table->buckets[i].lock);

    pthread_mutex_destroy(&table->change_lock);
}