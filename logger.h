#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define LOG_CAPACITY 128
#define LOG_MESSAGE_SIZE 192

typedef struct
{
    time_t time;

    char text[LOG_MESSAGE_SIZE];

} log_entry_t;

typedef struct
{
    FILE *file;

    pthread_t thread;

    pthread_mutex_t lock;

    pthread_cond_t changed;

    log_entry_t entries[LOG_CAPACITY];

    size_t head;

    size_t count;

    unsigned long dropped;

    int stopping;

} logger_t;

int logger_start(logger_t *log, const char *path);

void logger_write(logger_t *log, const char *format, ...);

void logger_stop(logger_t *log);

#endif
