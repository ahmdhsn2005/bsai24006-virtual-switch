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

static mac_entry_t *find_entry(
    mac_bucket_t *bucket,
    const uint8_t mac[6]
)
{
    mac_entry_t *e = bucket->head;

    while (e != NULL)
    {
        if (memcmp(e->mac, mac, 6) == 0)
            return e;

        e = e->next;
    }

    return NULL;
}

static void evict_lru(mac_table_t *table)
{
    mac_entry_t **oldest = NULL;

    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_lock(&table->buckets[i].lock);

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        mac_entry_t **link = &table->buckets[i].head;

        while (*link != NULL)
        {
            if (oldest == NULL ||
                (*link)->last_used < (*oldest)->last_used)
            {
                oldest = link;
            }

            link = &(*link)->next;
        }
    }

    if (oldest != NULL)
    {
        mac_entry_t *entry = *oldest;

        *oldest = entry->next;

        free(entry);

        table->size--;
    }

    for (int i = MAC_BUCKETS - 1; i >= 0; i--)
        pthread_mutex_unlock(&table->buckets[i].lock);
}

int mac_table_learn(mac_table_t *table, const uint8_t mac[6], int port)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;

    mac_bucket_t *bucket = &table->buckets[hash_mac(mac)];

    pthread_mutex_lock(&bucket->lock);

    mac_entry_t *e = find_entry(bucket, mac);

    if (e != NULL && e->port == port)
    {
        e->last_seen = now.tv_sec;

        e->last_used = atomic_fetch_add(&table->clock, 1);

        pthread_mutex_unlock(&bucket->lock);

        return 0;
    }

    pthread_mutex_unlock(&bucket->lock);

    pthread_mutex_lock(&table->change_lock);

    size_t count = count_on_port(table, port);

    pthread_mutex_lock(&bucket->lock);

    e = find_entry(bucket, mac);

    if (count >= MAC_MAX_PER_PORT &&
        (e == NULL || e->port != port))
    {
        pthread_mutex_unlock(&bucket->lock);

        pthread_mutex_unlock(&table->change_lock);

        return -2;
    }

    int previous_port = port;

    if (e == NULL)
    {
        e = malloc(sizeof(*e));

        if (e == NULL)
        {
            pthread_mutex_unlock(&bucket->lock);

            pthread_mutex_unlock(&table->change_lock);

            return -1;
        }

        if (table->size >= MAC_TABLE_LIMIT)
        {
            pthread_mutex_unlock(&bucket->lock);

            evict_lru(table);

            pthread_mutex_lock(&bucket->lock);
        }

        memcpy(e->mac, mac, 6);

        e->next = bucket->head;

        bucket->head = e;

        table->size++;
    }
    else
    {
        previous_port = e->port;
    }

    e->port = port;

    e->last_seen = now.tv_sec;

    e->last_used = atomic_fetch_add(&table->clock, 1);

    pthread_mutex_unlock(&bucket->lock);

    pthread_mutex_unlock(&table->change_lock);

    if (previous_port != port)
        printf("MAC moved: port %d -> port %d\n", previous_port, port);

    return 0;
}

int mac_table_lookup(
    mac_table_t *table,
    const uint8_t mac[6],
    int *port
)
{
    mac_bucket_t *bucket = &table->buckets[hash_mac(mac)];

    int found = -1;

    pthread_mutex_lock(&bucket->lock);

    mac_entry_t *e = find_entry(bucket, mac);

    if (e != NULL)
    {
        *port = e->port;

        e->last_used = atomic_fetch_add(&table->clock, 1);

        found = 0;
    }

    pthread_mutex_unlock(&bucket->lock);

    return found;
}

void mac_table_age(mac_table_t *table, unsigned int age_seconds)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
        perror("clock_gettime");

        return;
    }

    pthread_mutex_lock(&table->change_lock);

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t **link = &table->buckets[i].head;

        while (*link != NULL)
        {
            mac_entry_t *e = *link;

            if (now.tv_sec - e->last_seen >= (time_t)age_seconds)
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

void mac_table_remove_port(mac_table_t *table, int port)
{
    pthread_mutex_lock(&table->change_lock);

    for (int i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&table->buckets[i].lock);

        mac_entry_t **link = &table->buckets[i].head;

        while (*link != NULL)
        {
            mac_entry_t *e = *link;

            if (port < 0 || e->port == port)
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

void mac_table_clear(mac_table_t *table)
{
    mac_table_remove_port(table, -1);
}

void mac_table_destroy(mac_table_t *table)
{
    mac_table_clear(table);

    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_destroy(&table->buckets[i].lock);

    pthread_mutex_destroy(&table->change_lock);
}