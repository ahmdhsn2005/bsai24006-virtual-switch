#include "logger.h"

#include <stdarg.h>

static void *logger_main(void *argument)
{
    logger_t *log = argument;

    for (;;)
    {
        pthread_mutex_lock(&log->lock);

        while (log->count == 0 && !log->stopping)
            pthread_cond_wait(&log->changed, &log->lock);

        if (log->count == 0)
        {
            pthread_mutex_unlock(&log->lock);

            break;
        }

        log_entry_t entry = log->entries[log->head];

        unsigned long dropped = log->dropped;

        log->dropped = 0;

        log->head = (log->head + 1) % LOG_CAPACITY;

        log->count--;

        pthread_mutex_unlock(&log->lock);

        struct tm local;

        char stamp[32] = "unknown time";

        if (localtime_r(&entry.time, &local) != NULL &&
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local) == 0)
        {
            stamp[0] = '\0';
        }

        if (dropped > 0 &&
            fprintf(log->file, "[%s] %lu log messages dropped\n", stamp, dropped) < 0)
        {
            perror("log");
        }

        if (fprintf(log->file, "[%s] %s\n", stamp, entry.text) < 0 ||
            fflush(log->file) == EOF)
        {
            perror("log");
        }
    }

    return NULL;
}

int logger_start(logger_t *log, const char *path)
{
    log->file = fopen(path, "a");

    if (log->file == NULL)
        return -1;

    if (pthread_mutex_init(&log->lock, NULL) != 0)
    {
        fclose(log->file);

        return -1;
    }

    if (pthread_cond_init(&log->changed, NULL) != 0)
    {
        pthread_mutex_destroy(&log->lock);

        fclose(log->file);

        return -1;
    }

    if (pthread_create(&log->thread, NULL, logger_main, log) != 0)
    {
        pthread_cond_destroy(&log->changed);

        pthread_mutex_destroy(&log->lock);

        fclose(log->file);

        return -1;
    }

    return 0;
}

void logger_write(logger_t *log, const char *format, ...)
{
    log_entry_t entry;

    entry.time = time(NULL);

    va_list arguments;

    va_start(arguments, format);

    int length = vsnprintf(entry.text, sizeof(entry.text), format, arguments);

    va_end(arguments);

    if (length < 0)
        return;

    pthread_mutex_lock(&log->lock);

    if (log->count == LOG_CAPACITY)
    {
        log->dropped++;
    }
    else
    {
        size_t tail = (log->head + log->count) % LOG_CAPACITY;

        log->entries[tail] = entry;

        log->count++;

        pthread_cond_signal(&log->changed);
    }

    pthread_mutex_unlock(&log->lock);
}

void logger_stop(logger_t *log)
{
    pthread_mutex_lock(&log->lock);

    log->stopping = 1;

    pthread_cond_signal(&log->changed);

    pthread_mutex_unlock(&log->lock);

    pthread_join(log->thread, NULL);

    if (fclose(log->file) == EOF)
        perror("log");

    pthread_cond_destroy(&log->changed);

    pthread_mutex_destroy(&log->lock);
}
