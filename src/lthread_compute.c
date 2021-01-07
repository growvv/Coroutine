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
 * lthread_compute.c
 */

#include <sys/queue.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <pthread.h>

#include "lthread_int.h"

enum {THREAD_TIMEOUT_BEFORE_EXIT = 60};
static pthread_key_t compute_sched_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

LIST_HEAD(compute_sched_l, lthread_compute_sched) compute_scheds = \
    LIST_HEAD_INITIALIZER(compute_scheds);                 // lmy: 存放compute sched的链表
pthread_mutex_t sched_mutex = PTHREAD_MUTEX_INITIALIZER;

static void* _lthread_compute_run(void *arg);
static void _lthread_compute_resume(struct lthread *lt);
static struct lthread_compute_sched* _lthread_compute_sched_create(void);
static void _lthread_compute_sched_free(
    struct lthread_compute_sched *compute_sched);

struct lthread_compute_sched {
    struct cpu_ctx      ctx;
    struct lthread_q    lthreads;
    struct lthread      *current_lthread;
    pthread_mutex_t     run_mutex;
    pthread_cond_t      run_mutex_cond;
    pthread_mutex_t     lthreads_mutex;
    LIST_ENTRY(lthread_compute_sched)    compute_next;
    enum lthread_compute_st compute_st;                    // [lmy] compute sched的状态：空闲或忙碌
};

// [lmy]获取一个compute sched或者直接创建一个载有compute sched的新线程，
// 将当前lthread交由compute sched运行，并让出当前lthread占有的线程。
// NOTE：此即如何将一个lthread扔到另外一个线程上执行，只需把lthread信息复制到另一个线程的调度器上就可以了
int
lthread_compute_begin(void)
{
    struct lthread_sched *sched = lthread_get_sched();       
    struct lthread_compute_sched *compute_sched = NULL, *tmp = NULL;
    struct lthread *lt = sched->current_lthread;            // [lmy] 获取执行代码自身的lthread信息

    /* search for an empty compute_scheduler */
    assert(pthread_mutex_lock(&sched_mutex) == 0);
    LIST_FOREACH(tmp, &compute_scheds, compute_next) {    // [lmy] 宏定义的for循环
        if (tmp->compute_st == LT_COMPUTE_FREE) {
            compute_sched = tmp;
            break;
        }
    }

    /* create schedule if there is no scheduler available */
    if (compute_sched == NULL) {
        if ((compute_sched = _lthread_compute_sched_create()) == NULL) {  // [lmy] 若在这里创建了一个载有compute sched的pthread，它马上就会进入调度循环
            /* we failed to create a scheduler. Use the first scheduler
             * in the list, otherwise return failure.
             */
            compute_sched = LIST_FIRST(&compute_scheds);
            if (compute_sched == NULL) {
                assert(pthread_mutex_unlock(&sched_mutex) == 0);
                return -1;
            }
        } else {
            LIST_INSERT_HEAD(&compute_scheds, compute_sched, compute_next);     // [lmy] 若创建成功则插入到compute sched链表中
        }
    }

    lt->compute_sched = compute_sched;      // [lmy] 注册自身的compute sched信息

    lt->state |= BIT(LT_ST_PENDING_RUNCOMPUTE);     
    assert(pthread_mutex_lock(&lt->compute_sched->lthreads_mutex) == 0);
    TAILQ_INSERT_TAIL(&lt->compute_sched->lthreads, lt, compute_next);   // [lmy]把当前lthread的地址插入compute sched的队列尾部，但PENDING_RUNCOMPUTE状态还不能马上被执行
    assert(pthread_mutex_unlock(&lt->compute_sched->lthreads_mutex) == 0);

    assert(pthread_mutex_unlock(&sched_mutex) == 0);

    /* yield function in scheduler to allow other lthreads to run while
     * this lthread runs in a pthread for expensive computations.
     */
    _switch(&lt->sched->ctx, &lt->ctx);     // [lmy] 让出当前占用的线程，切换到调度器会继续执行lhread_resume剩下的指令，
                                            // 包括转移PENDING_RUNCOMPUTE状态为LT_ST_RUNCOMPUTE，一旦修改compute sched就可以执行这个lthread了

    return (0);
}

// [lmy] 相当于compute sched上的yield，让出compute sched所在的线程
void
lthread_compute_end(void)
{
    /* get current compute scheduler */
    struct lthread_compute_sched *compute_sched =
        pthread_getspecific(compute_sched_key);
    struct lthread *lt = compute_sched->current_lthread;
    assert(compute_sched != NULL);
    _switch(&compute_sched->ctx, &lt->ctx);
}

// [lmy] 这个函数被将要转移到compute sched上执行的那个lthread的原所属sched执行，正式确认lthread的转移
void
_lthread_compute_add(struct lthread *lt)
{

    LIST_INSERT_HEAD(&lt->sched->busy, lt, busy_next);  // [lmy] 将lthread注册到原sched的busy链表上
    /*
     * lthread is in scheduler list at this point. lock mutex to change
     * state since the state is checked in scheduler as well.
     */
    assert(pthread_mutex_lock(&lt->compute_sched->lthreads_mutex) == 0);
    lt->state &= CLEARBIT(LT_ST_PENDING_RUNCOMPUTE);    // [lmy] PENDING_RUNCOMPUTE状态还不能被compute sched执行
    lt->state |= BIT(LT_ST_RUNCOMPUTE);                 // [lmy] 只有lthread原所属sched进一步把lthread的状态从PENDING_RUNCOMPUTE转移至RUNCOMPUTE，compute sched才可以执行它
    assert(pthread_mutex_unlock(&lt->compute_sched->lthreads_mutex) == 0);

    /* wakeup pthread if it was sleeping */
    assert(pthread_mutex_lock(&lt->compute_sched->run_mutex) == 0);
    assert(pthread_cond_signal(&lt->compute_sched->run_mutex_cond) == 0);   // [lmy] 若compute sched没有任务，它会通过pthread_cond_timedwait等待60秒，
                                                            // 这里创建了任务后及时唤醒它，防止其exit（对应的cond_timedwait在_lthread_compute_run）
    assert(pthread_mutex_unlock(&lt->compute_sched->run_mutex) == 0);

}

static void
_lthread_compute_sched_free(struct lthread_compute_sched *compute_sched)
{
    assert(pthread_mutex_destroy(&compute_sched->run_mutex) == 0);
    assert(pthread_mutex_destroy(&compute_sched->lthreads_mutex) == 0);
    assert(pthread_cond_destroy(&compute_sched->run_mutex_cond) == 0);
    free(compute_sched);
}

// [lmy]在进程的堆上创建一个compute sched的结构体，创建一个pthread，并为其绑定compute sched的调度循环函数
// 创建成功返回compute sched的地址，失败返回NULL
static struct lthread_compute_sched*
_lthread_compute_sched_create(void)
{
    struct lthread_compute_sched *compute_sched = NULL;     
    pthread_t pthread;

    if ((compute_sched = calloc(1,                  
        sizeof(struct lthread_compute_sched))) == NULL)     // [lmy]compute sched的信息位于进程的堆中
        return NULL;

    if (pthread_mutex_init(&compute_sched->run_mutex, NULL) != 0 ||
        pthread_mutex_init(&compute_sched->lthreads_mutex, NULL) != 0 ||
        pthread_cond_init(&compute_sched->run_mutex_cond, NULL) != 0) {
        free(compute_sched);
        return NULL;
    }

    if (pthread_create(&pthread,
        NULL, _lthread_compute_run, compute_sched) != 0) {
        _lthread_compute_sched_free(compute_sched);
        return NULL;
    }
    assert(pthread_detach(pthread) == 0);

    TAILQ_INIT(&compute_sched->lthreads);       // [lmy]初始化lthread队列为一个空队列

    return compute_sched;
}

// compute sched开始执行某一个lthread
static void
_lthread_compute_resume(struct lthread *lt)
{
    _switch(&lt->ctx, &lt->compute_sched->ctx);
}

// 
static void
once_routine(void)
{
    assert(pthread_key_create(&compute_sched_key, NULL) == 0);
}

// computer sched的调度循环，参数arg是调度器
static void*
_lthread_compute_run(void *arg)
{
    struct lthread_compute_sched *compute_sched = arg;      
    struct lthread *lt = NULL;
    struct timespec timeout;
    int status = 0;
    int ret = 0;
    (void)ret; /* silence compiler */

    assert(pthread_once(&key_once, once_routine) == 0);
    assert(pthread_setspecific(compute_sched_key, arg) == 0);

    while (1) { // 外循环用来判断是否需要exit，会调用一个等待

        /* resume lthreads to run their computation or make a blocking call */
        while (1) {     // [lmy] compute sched要干的活在这里
            assert(pthread_mutex_lock(&compute_sched->lthreads_mutex) == 0);

            /* we have no work to do, break and wait 60 secs then exit */
            if (TAILQ_EMPTY(&compute_sched->lthreads)) {
                assert(pthread_mutex_unlock(
                    &compute_sched->lthreads_mutex) == 0);
                break;
            }

            lt = TAILQ_FIRST(&compute_sched->lthreads);
            if (lt->state & BIT(LT_ST_PENDING_RUNCOMPUTE)) {
                assert(pthread_mutex_unlock(
                    &compute_sched->lthreads_mutex) == 0);
                continue;
            }

            TAILQ_REMOVE(&compute_sched->lthreads, lt, compute_next);   // 【直接从队列中移除，所以lthread一旦yield就不会再回来执行了？需要思考一下为什么作这样的设定……】

            assert(pthread_mutex_unlock(&compute_sched->lthreads_mutex) == 0);

            compute_sched->current_lthread = lt;
            compute_sched->compute_st = LT_COMPUTE_BUSY;    // 调整调度器状态为：正忙

            _lthread_compute_resume(lt);                    // 执行lthread

            compute_sched->current_lthread = NULL;
            compute_sched->compute_st = LT_COMPUTE_FREE;

            /* resume it back on the  prev scheduler */
            assert(pthread_mutex_lock(&lt->sched->defer_mutex) == 0);   // 执行完了这个ltherad后，还要把它还给原来的sched
            TAILQ_INSERT_TAIL(&lt->sched->defer, lt, defer_next);       // NOTE: defer状态在这里!
            lt->state &= CLEARBIT(LT_ST_RUNCOMPUTE);
            assert(pthread_mutex_unlock(&lt->sched->defer_mutex) == 0);

            /* signal the prev scheduler in case it was sleeping in a poll */
            _lthread_poller_ev_trigger(lt->sched);      // NOTE：调度器可能阻塞在epoll_wait上，如果这里的计算执行完了，要让原调度器及时醒过来
        }

        assert(pthread_mutex_lock(&compute_sched->run_mutex) == 0);
        /* wait if we have no work to do, exit */
        timeout.tv_sec = time(NULL) + THREAD_TIMEOUT_BEFORE_EXIT;
        timeout.tv_nsec = 0;
        status = pthread_cond_timedwait(&compute_sched->run_mutex_cond,
            &compute_sched->run_mutex, &timeout);
        assert(pthread_mutex_unlock(&compute_sched->run_mutex) == 0);

        /* if we didn't timeout, then we got signaled to do some work */
        if (status != ETIMEDOUT)        // [lmy] 这表明某个pthread丢了一个任务过来，于是compute sched有活干了
            continue;

        /* lock the global sched to check if we have any pending work to do */
        assert(pthread_mutex_lock(&sched_mutex) == 0);

        assert(pthread_mutex_lock(&compute_sched->lthreads_mutex) == 0);
        if (TAILQ_EMPTY(&compute_sched->lthreads)) {

            LIST_REMOVE(compute_sched, compute_next);

            assert(pthread_mutex_unlock(&compute_sched->lthreads_mutex) == 0);
            assert(pthread_mutex_unlock(&sched_mutex) == 0);
            _lthread_compute_sched_free(compute_sched);
            break;
        }

        assert(pthread_mutex_unlock(&compute_sched->lthreads_mutex) == 0);
        assert(pthread_mutex_unlock(&sched_mutex) == 0);
    }


    return NULL;
}
