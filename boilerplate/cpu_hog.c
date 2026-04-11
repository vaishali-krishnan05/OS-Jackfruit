/*
 * cpu_hog.c - Multithreaded CPU-bound workload for scheduler experiments.
 *
 * Usage:
 *   ./cpu_hog [seconds] [threads]
 *
 * Defaults: 60 seconds, 16 threads.
 * Spawn more threads than available cores to guarantee CPU contention
 * even on multi-core machines.
 *
 * Each thread burns CPU in a tight arithmetic loop and reports progress
 * every second so you can observe scheduling behavior in the logs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <string.h>

#define DEFAULT_SECONDS 60
#define DEFAULT_THREADS 16

typedef struct {
    int thread_id;
    unsigned int duration;
    unsigned long long iterations; /* filled in on exit */
} thread_arg_t;

static void *worker(void *arg)
{
    thread_arg_t *a = (thread_arg_t *)arg;
    const time_t start = time(NULL);
    volatile unsigned long long acc = (unsigned long long)a->thread_id * 6364136223846793005ULL;
    unsigned long long iters = 0;

    while ((unsigned int)(time(NULL) - start) < a->duration) {
        /* tight arithmetic — keeps the core fully busy */
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= (acc >> 33);
        iters++;
    }

    a->iterations = iters;
    return NULL;
}

int main(int argc, char *argv[])
{
    unsigned int duration = DEFAULT_SECONDS;
    int nthreads          = DEFAULT_THREADS;

    if (argc > 1) {
        duration = (unsigned int)strtoul(argv[1], NULL, 10);
        if (duration == 0) duration = DEFAULT_SECONDS;
    }
    if (argc > 2) {
        nthreads = (int)strtol(argv[2], NULL, 10);
        if (nthreads <= 0) nthreads = DEFAULT_THREADS;
    }

    printf("cpu_hog starting: threads=%d duration=%us\n", nthreads, duration);
    fflush(stdout);

    pthread_t     *threads = malloc((size_t)nthreads * sizeof(pthread_t));
    thread_arg_t  *args    = malloc((size_t)nthreads * sizeof(thread_arg_t));
    if (!threads || !args) {
        fprintf(stderr, "cpu_hog: malloc failed\n");
        return 1;
    }

    const time_t start = time(NULL);
    time_t last_report = start;

    /* Launch all worker threads */
    for (int i = 0; i < nthreads; i++) {
        args[i].thread_id = i;
        args[i].duration  = duration;
        args[i].iterations = 0;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    /* Main thread prints progress once per second */
    while ((unsigned int)(time(NULL) - start) < duration) {
        if (time(NULL) != last_report) {
            last_report = time(NULL);
            printf("cpu_hog alive elapsed=%ld threads=%d\n",
                   (long)(last_report - start), nthreads);
            fflush(stdout);
        }
    }

    /* Join all threads */
    unsigned long long total_iters = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
        total_iters += args[i].iterations;
    }

    printf("cpu_hog done duration=%u threads=%d total_iterations=%llu\n",
           duration, nthreads, total_iters);
    fflush(stdout);

    free(threads);
    free(args);
    return 0;
}
