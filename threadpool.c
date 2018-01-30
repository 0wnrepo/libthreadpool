#include "threadpool.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define NCORES (sysconf(_SC_NPROCESSORS_ONLN))

typedef struct job_list {
    void (*func)(void *);
    void *arg;
    struct job_list *next;
} job_list;

struct threadpool {
    size_t nthreads;
    pthread_t *threads;
    size_t num_active_jobs;
    job_list *first_job;
    job_list *last_job;
    pthread_mutex_t job_list_lock;
    pthread_mutex_t worker_wakeup_lock;
    pthread_cond_t  worker_wakeup_cond;
    bool flag_exit_please;
};

static void * threadpool_worker(void *arg);

threadpool *
threadpool_create(size_t nthreads)
{
    threadpool *pool;

    if ((pool = calloc(1, sizeof pool[0])) == NULL)
        return NULL;

    pool->nthreads = nthreads;
    if ((pool->threads = calloc(nthreads, sizeof pool->threads[0])) == NULL)
        goto error;

    pool->num_active_jobs = 0;
    pool->first_job = NULL;
    pool->last_job = NULL;

    pthread_mutex_init(&pool->job_list_lock, NULL);
    pthread_mutex_init(&pool->worker_wakeup_lock, NULL);
    pthread_cond_init(&pool->worker_wakeup_cond, NULL);

    pool->flag_exit_please = false;
    for (size_t i = 0; i < nthreads; i++)
        (void) pthread_create(&pool->threads[i], NULL, threadpool_worker, pool);
    return pool;
error:
    if (pool)
        free(pool);
    return NULL;
}

void
threadpool_destroy(threadpool *pool)
{
    if (pool == NULL)
        return;
    pthread_mutex_lock(&pool->job_list_lock);
    pool->flag_exit_please = true;
    pthread_mutex_unlock(&pool->job_list_lock);
    pthread_cond_broadcast(&pool->worker_wakeup_cond);
    threadpool_wait(pool);

    for (size_t i = 0; i < pool->nthreads; i++)
        (void) pthread_join(pool->threads[i], NULL);
    assert(pool->num_active_jobs == 0); // we should be done with all jobs now
    free(pool->threads);

    assert(pool->first_job == NULL); // there should be no jobs left in the queue
    assert(pool->last_job == NULL);

    pthread_mutex_destroy(&pool->job_list_lock);
    pthread_mutex_destroy(&pool->worker_wakeup_lock);
    pthread_cond_destroy(&pool->worker_wakeup_cond);

    free(pool);
}

int
threadpool_add_job(threadpool *pool, void (*func)(void *), void *arg)
{
    job_list *new, *tmp;

    if ((new = calloc(1, sizeof new[0])) == NULL)
        return THREADPOOL_ERR;
    new->func = func;
    new->arg  = arg;
    new->next = NULL;

    pthread_mutex_lock(&pool->job_list_lock);
    {
        if (pool->first_job == NULL) {
            pool->first_job = pool->last_job = new;
        } else {
            tmp = pool->last_job;
            pool->last_job = tmp->next = new;
        }
        pool->num_active_jobs++;
    }
    pthread_mutex_unlock(&pool->job_list_lock);
    pthread_cond_signal(&pool->worker_wakeup_cond);
    return THREADPOOL_OK;
}

void
threadpool_wait(threadpool *pool)
{
    pthread_mutex_lock(&pool->job_list_lock);
    while (pool->num_active_jobs > 0) {
        pthread_mutex_unlock(&pool->job_list_lock);
        pthread_cond_broadcast(&pool->worker_wakeup_cond);
        pthread_mutex_lock(&pool->job_list_lock);
    }
    pthread_mutex_unlock(&pool->job_list_lock);
}

static bool
job_available(const threadpool *pool)
{
    /* assumes we have the pool->job_list_lock lock */
    return pool->first_job != NULL;
}

static void *
threadpool_worker(void *pool_)
{
    threadpool *pool = pool_;
    while (1) {
        pthread_mutex_lock(&pool->job_list_lock);
        if (job_available(pool)) {
            job_list *node;
            node = pool->first_job;
            pool->first_job = node->next;
            if (pool->first_job == NULL)
                pool->last_job = NULL;
            pthread_mutex_unlock(&pool->job_list_lock);

            node->func(node->arg);
            free(node);

            pthread_mutex_lock(&pool->job_list_lock);
            pool->num_active_jobs--;
            pthread_mutex_unlock(&pool->job_list_lock);
        } else {
            pthread_mutex_unlock(&pool->job_list_lock);
            if (pool->flag_exit_please)
                pthread_exit(NULL);
            pthread_mutex_lock(&pool->worker_wakeup_lock);
            pthread_mutex_lock(&pool->job_list_lock);
            if (job_available(pool) || pool->flag_exit_please) {
                pthread_mutex_unlock(&pool->job_list_lock);
            } else {
                pthread_mutex_unlock(&pool->job_list_lock);
                pthread_cond_wait(&pool->worker_wakeup_cond, &pool->worker_wakeup_lock);
            }
            pthread_mutex_unlock(&pool->worker_wakeup_lock);
        }
    }
}
