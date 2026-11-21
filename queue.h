#ifndef QUEUE_H
#define QUEUE_H

#include "frame.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

#define PORT_QUEUE_CAPACITY 64

#define HIGH_PRIORITY 0
#define NORMAL_PRIORITY 1
#define LOW_PRIORITY 2

typedef struct shared_frame
{
    atomic_uint references;

    frame_t frame;

} shared_frame_t;

typedef struct queue_node
{
    shared_frame_t *item;

    struct timespec queued_at;

    struct queue_node *next;

} queue_node_t;

typedef struct
{
    queue_node_t *head[3];

    queue_node_t *tail[3];

    size_t count;

    pthread_mutex_t lock;

    pthread_cond_t changed;

    int closed;

    int draining;

    unsigned long generation;

    unsigned int schedule_index;

    double tokens;

    double token_capacity;

    double rate_per_second;

    struct timespec last_refill;

} output_queue_t;

shared_frame_t *shared_frame_create(const frame_t *frame);

void shared_frame_retain(shared_frame_t *frame);

void shared_frame_release(shared_frame_t *frame);

int output_queue_init(output_queue_t *queue, double rate);

int output_queue_push(output_queue_t *queue, shared_frame_t *frame);

shared_frame_t *output_queue_pop(
    output_queue_t *queue,
    unsigned long *generation
);

int output_queue_current(output_queue_t *queue, unsigned long generation);

void output_queue_refill(output_queue_t *queue);

size_t output_queue_configure(output_queue_t *queue, double rate, int discard);

void output_queue_finish(output_queue_t *queue);

void output_queue_close(output_queue_t *queue);

void output_queue_destroy(output_queue_t *queue);

#endif