/*
 * Lthread
 * Copyright (C) 2012, Hasan Alayli <halayli@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * lthread_io.c
 */

#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "lthread_int.h"

#define IO_WORKERS 2

static void _lthread_io_add(struct lthread *lt);
static void *_lthread_io_worker(void *arg);

static pthread_once_t key_once = PTHREAD_ONCE_INIT;

struct lthread_io_worker {
    struct lthread_q    lthreads;
    pthread_mutex_t     run_mutex;
    pthread_cond_t      run_mutex_cond;
    pthread_mutex_t     lthreads_mutex;
};

static struct lthread_io_worker io_workers[IO_WORKERS];

// 初始化两个io_worker的信息，并创建两个载有_lthread_io_worker的线程
// 注：此函数在使用时只会被执行一次
static void
once_routine(void)
{
    pthread_t pthread;
    struct lthread_io_worker *io_worker = NULL;
    int i = 0;

    for (i = 0; i < IO_WORKERS; i++) {
        io_worker = &io_workers[i];

        assert(pthread_mutex_init(&io_worker->lthreads_mutex, NULL) == 0);
        assert(pthread_mutex_init(&io_worker->run_mutex, NULL) == 0);
        assert(pthread_create(&pthread,
            NULL, _lthread_io_worker, io_worker) == 0);
        TAILQ_INIT(&io_worker->lthreads);

    }
}

void
_lthread_io_worker_init()
{
    assert(pthread_once(&key_once, once_routine) == 0);     // pthread_once和key_once保证once_routine函数只会被调用一次
}

// io_worker上的主程序，类似compute sched的调度器
// arg是某个io_worker的信息（结构体指针）
static void *
_lthread_io_worker(void *arg)
{
    struct lthread_io_worker *io_worker = arg;
    struct lthread *lt = NULL;

    assert(pthread_once(&key_once, once_routine) == 0);     // 完成两个io_worker的信息初始化和线程创建工作，如果它们还没有被做过

    while (1) {

        while (1) {
            assert(pthread_mutex_lock(&io_worker->lthreads_mutex) == 0);

            /* we have no work to do, break and wait */
            if (TAILQ_EMPTY(&io_worker->lthreads)) {
                assert(pthread_mutex_unlock(&io_worker->lthreads_mutex) == 0);
                break;
            }

            lt = TAILQ_FIRST(&io_worker->lthreads);
            TAILQ_REMOVE(&io_worker->lthreads, lt, io_next);

            assert(pthread_mutex_unlock(&io_worker->lthreads_mutex) == 0);

            if (lt->state & BIT(LT_ST_WAIT_IO_READ)) {
                lt->io.ret = read(lt->io.fd, lt->io.buf, lt->io.nbytes);
                lt->io.err = (lt->io.ret == -1) ? errno : 0;
            } else if (lt->state & BIT(LT_ST_WAIT_IO_WRITE)) {
                lt->io.ret = write(lt->io.fd, lt->io.buf, lt->io.nbytes);
                lt->io.err = (lt->io.ret == -1) ? errno : 0;
            } else
                assert(0);

            /* resume it back on the  prev scheduler */
            assert(pthread_mutex_lock(&lt->sched->defer_mutex) == 0);
            TAILQ_INSERT_TAIL(&lt->sched->defer, lt, defer_next);       // io完成之后把lt注册到原sched的defer队列中
            assert(pthread_mutex_unlock(&lt->sched->defer_mutex) == 0);

            /* signal the prev scheduler in case it was sleeping in a poll */
            _lthread_poller_ev_trigger(lt->sched);   // 同compute一样，如果原调度器阻塞在epoll_wait上，此处的io完毕后应该它们及时醒过来              
        }

        assert(pthread_mutex_lock(&io_worker->run_mutex) == 0);
        pthread_cond_wait(&io_worker->run_mutex_cond,
            &io_worker->run_mutex);         // 暂时没有io工作要做，整个线程wait
        assert(pthread_mutex_unlock(&io_worker->run_mutex) == 0);

    }

}

// 被普通的协程执行，将其上的某个lt放进busy队列，并注册到一个io_worker线程上，
// 如果该io_worker处于睡眠状态（没有任务而wait中）就唤醒它；注册完毕后yield
static void
_lthread_io_add(struct lthread *lt)
{
    static uint32_t io_selector = 0;
    struct lthread_io_worker *io_worker = &io_workers[io_selector++];
    io_selector = io_selector % IO_WORKERS;

    LIST_INSERT_HEAD(&lt->sched->busy, lt, busy_next);

    assert(pthread_mutex_lock(&io_worker->lthreads_mutex) == 0);
    TAILQ_INSERT_TAIL(&io_worker->lthreads, lt, io_next);
    assert(pthread_mutex_unlock(&io_worker->lthreads_mutex) == 0);

    /* wakeup pthread if it was sleeping */
    assert(pthread_mutex_lock(&io_worker->run_mutex) == 0);
    assert(pthread_cond_signal(&io_worker->run_mutex_cond) == 0);
    assert(pthread_mutex_unlock(&io_worker->run_mutex) == 0);

    _lthread_yield(lt);

    /* restore errno we got from io worker, if any */
    if (lt->io.ret == -1)       // 从io_worker执行回来后，检查一下io是否成功
        errno = lt->io.err;
}

// 被普通协程执行，会调用_thread_io_add将自己放到一个io_worker线程上去做io；tests/lthread_io.c中示范了lthread_io_write的使用
// NOTE: 把io放到专用的io_worker上去执行使得lthread会被阻塞但并不耽误其它lthread的执行，这正是pthread的特点
ssize_t
lthread_io_read(int fd, void *buf, size_t nbytes)
{
    struct lthread *lt = lthread_get_sched()->current_lthread;
    lt->state |= BIT(LT_ST_WAIT_IO_READ);
    lt->io.buf = buf;
    lt->io.fd = fd;
    lt->io.nbytes = nbytes;

    _lthread_io_add(lt);
    lt->state &= CLEARBIT(LT_ST_WAIT_IO_READ);

    return (lt->io.ret);
}

// 被普通协程执行，会调用_thread_io_add将自己放到一个io_worker线程上去做io；tests/lthread_io.c中示范了lthread_io_write的使用
// NOTE: 把io放到专用的io_worker上去执行使得lthread会被阻塞但并不耽误其它lthread的执行，这正是pthread的特点
ssize_t
lthread_io_write(int fd, void *buf, size_t nbytes)
{
    struct lthread *lt = lthread_get_sched()->current_lthread;
    lt->state |= BIT(LT_ST_WAIT_IO_WRITE);
    lt->io.buf = buf;
    lt->io.nbytes = nbytes;
    lt->io.fd = fd;

    _lthread_io_add(lt);
    lt->state &= CLEARBIT(LT_ST_WAIT_IO_WRITE);

    return (lt->io.ret);
}
