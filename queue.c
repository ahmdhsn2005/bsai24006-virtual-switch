#include "queue.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>

static const int schedule[9] = {
    HIGH_PRIORITY,
    HIGH_PRIORITY,
    HIGH_PRIORITY,
    HIGH_PRIORITY,
    HIGH_PRIORITY,

    NORMAL_PRIORITY,
    NORMAL_PRIORITY,
    NORMAL_PRIORITY,

    LOW_PRIORITY
};

static double elapsed_seconds(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec) +
           (double)(b.tv_nsec - a.tv_nsec) / 1000000000.0;
}

shared_frame_t *shared_frame_create(const frame_t *frame)
{
    shared_frame_t *shared = malloc(sizeof(*shared));

    if (shared == NULL)
        return NULL;

    atomic_init(&shared->references, 1);

    memcpy(&shared->frame, frame, sizeof(*frame));

    return shared;
}

void shared_frame_retain(shared_frame_t *frame)
{
    atomic_fetch_add(&frame->references, 1);
}

void shared_frame_release(shared_frame_t *frame)
{
    if (atomic_fetch_sub(&frame->references, 1) == 1)
        free(frame);
}

int output_queue_init(output_queue_t *queue, double rate)
{
    memset(queue, 0, sizeof(*queue));

    if (clock_gettime(CLOCK_MONOTONIC, &queue->last_refill) < 0)
        return -1;

    if (pthread_mutex_init(&queue->lock, NULL) != 0)
        return -1;

    pthread_condattr_t attributes;

    if (pthread_condattr_init(&attributes) != 0)
    {
        pthread_mutex_destroy(&queue->lock);

        return -1;
    }

    int result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);

    if (result == 0)
        result = pthread_cond_init(&queue->changed, &attributes);

    pthread_condattr_destroy(&attributes);

    if (result != 0)
    {
        pthread_mutex_destroy(&queue->lock);

        return -1;
    }

    queue->rate_per_second = rate;

    queue->token_capacity = rate;

    queue->tokens = rate;

    return 0;
}

int output_queue_push(output_queue_t *queue, shared_frame_t *frame)
{
    int priority = frame->frame.priority;

    queue_node_t *node = malloc(sizeof(*node));

    if (node == NULL)
        return -1;

    node->item = frame;

    node->next = NULL;

    if (clock_gettime(CLOCK_MONOTONIC, &node->queued_at) < 0)
    {
        free(node);

        return -1;
    }

    pthread_mutex_lock(&queue->lock);

    if (queue->closed || queue->draining ||
        queue->count >= PORT_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&queue->lock);

        free(node);

        return -1;
    }

    if (queue->tail[priority] == NULL)
    {
        queue->head[priority] = node;
    }
    else
    {
        queue->tail[priority]->next = node;
    }

    queue->tail[priority] = node;

    queue->count++;

    pthread_cond_broadcast(&queue->changed);

    pthread_mutex_unlock(&queue->lock);

    return 0;
}

static int aged_priority(output_queue_t *queue)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;

    int selected = -1;

    double oldest = 2.0;

    for (int p = 0; p < 3; p++)
    {
        if (queue->head[p] != NULL)
        {
            double age = elapsed_seconds(
                queue->head[p]->queued_at,
                now
            );

            if (age >= oldest)
            {
                oldest = age;

                selected = p;
            }
        }
    }

    return selected;
}

static int choose_priority(output_queue_t *queue)
{
    int aged = aged_priority(queue);

    if (aged >= 0)
        return aged;

    for (int i = 0; i < 9; i++)
    {
        int p = schedule[queue->schedule_index];

        queue->schedule_index =
            (queue->schedule_index + 1) % 9;

        if (queue->head[p] != NULL)
            return p;
    }

    for (int p = 0; p < 3; p++)
    {
        if (queue->head[p] != NULL)
            return p;
    }

    return -1;
}

static void refill_locked(output_queue_t *queue, struct timespec now)
{
    double elapsed = elapsed_seconds(queue->last_refill, now);

    queue->tokens += elapsed * queue->rate_per_second;

    if (queue->tokens > queue->token_capacity)
        queue->tokens = queue->token_capacity;

    queue->last_refill = now;
}

shared_frame_t *output_queue_pop(
    output_queue_t *queue,
    unsigned long *generation
)
{
    pthread_mutex_lock(&queue->lock);

    while (!queue->closed)
    {
        if (queue->count == 0)
        {
            if (queue->draining ||
                pthread_cond_wait(&queue->changed, &queue->lock) != 0)
            {
                break;
            }

            continue;
        }

        struct timespec deadline;

        if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
            break;

        refill_locked(queue, deadline);

        if (queue->tokens >= 1.0)
        {
            int p = choose_priority(queue);

            queue_node_t *node = queue->head[p];

            queue->head[p] = node->next;

            if (queue->head[p] == NULL)
                queue->tail[p] = NULL;

            queue->count--;

            queue->tokens -= 1.0;

            shared_frame_t *frame = node->item;

            *generation = queue->generation;

            free(node);

            pthread_mutex_unlock(&queue->lock);

            return frame;
        }

        deadline.tv_nsec += (long)((1.0 - queue->tokens) /
                                   queue->rate_per_second * 1000000000.0) + 1;

        deadline.tv_sec += deadline.tv_nsec / 1000000000;

        deadline.tv_nsec %= 1000000000;

        int result = pthread_cond_timedwait(
            &queue->changed,
            &queue->lock,
            &deadline
        );

        if (result != 0 && result != ETIMEDOUT)
            break;
    }

    pthread_mutex_unlock(&queue->lock);

    return NULL;
}

int output_queue_current(output_queue_t *queue, unsigned long generation)
{
    pthread_mutex_lock(&queue->lock);

    int current = !queue->closed && queue->generation == generation;

    pthread_mutex_unlock(&queue->lock);

    return current;
}

void output_queue_refill(output_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);

    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
    {
        refill_locked(queue, now);

        if (queue->count > 0)
            pthread_cond_signal(&queue->changed);
    }

    pthread_mutex_unlock(&queue->lock);
}

static void discard_frames(output_queue_t *queue)
{
    for (int p = 0; p < 3; p++)
    {
        queue_node_t *node = queue->head[p];

        while (node != NULL)
        {
            queue_node_t *next = node->next;

            shared_frame_release(node->item);

            free(node);

            node = next;
        }

        queue->head[p] = NULL;

        queue->tail[p] = NULL;
    }

    queue->count = 0;
}

size_t output_queue_configure(output_queue_t *queue, double rate, int discard)
{
    pthread_mutex_lock(&queue->lock);

    size_t dropped = 0;

    if (discard)
    {
        dropped = queue->count;

        discard_frames(queue);

        queue->generation++;
    }

    if (rate != queue->rate_per_second)
    {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
            refill_locked(queue, now);

        queue->rate_per_second = rate;

        queue->token_capacity = rate;

        if (queue->tokens > rate)
            queue->tokens = rate;
    }

    pthread_cond_signal(&queue->changed);

    pthread_mutex_unlock(&queue->lock);

    return dropped;
}

void output_queue_finish(output_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);

    queue->draining = 1;

    pthread_cond_signal(&queue->changed);

    pthread_mutex_unlock(&queue->lock);
}

void output_queue_close(output_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);

    queue->closed = 1;

    pthread_cond_signal(&queue->changed);

    pthread_mutex_unlock(&queue->lock);
}

void output_queue_destroy(output_queue_t *queue)
{
    discard_frames(queue);

    pthread_cond_destroy(&queue->changed);

    pthread_mutex_destroy(&queue->lock);
}
