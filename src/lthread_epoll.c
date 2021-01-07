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
 * lthread_epoll.c
 */

#include "lthread_epoll.h"

// 封装epoll_create
// 创建一个epoll实例
int
_lthread_poller_create(void)
{
    return (epoll_create(1024));
}

// 调用epoll_wait, 获取就绪的epoll_event个数
inline int
_lthread_poller_poll(struct timespec t)
{
    struct lthread_sched *sched = lthread_get_sched();

    return (epoll_wait(sched->poller_fd, sched->eventlist, LT_MAX_EVENTS,
        t.tv_sec*1000.0 + t.tv_nsec/1000000.0));    // 调度器自身可能会在这里阻塞，_lthread_poller_ev_trigger唤醒的就是这个时候的调度器！！！
}

// 注销一个监听读类型事件的文件描述符
inline void
_lthread_poller_ev_clear_rd(int fd)
{
    struct epoll_event ev;
    int ret = 0;
    struct lthread_sched *sched = lthread_get_sched();

    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_DEL, fd, &ev);
    assert(ret != -1);
}

// 注销一个监听写类型事件的文件描述符
inline void
_lthread_poller_ev_clear_wr(int fd)
{
    struct epoll_event ev;
    int ret = 0;
    struct lthread_sched *sched = lthread_get_sched();

    ev.data.fd = fd;
    ev.events = EPOLLOUT | EPOLLONESHOT | EPOLLRDHUP;
    ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_DEL, fd, &ev);
    assert(ret != -1);
}

// 将fd注册到epoll实例中（如果还没有的话），fd的感兴趣事件为三个read相关的事件 
inline void
_lthread_poller_ev_register_rd(int fd)
{
    struct epoll_event ev;
    int ret = 0;
    struct lthread_sched *sched = lthread_get_sched();

    // EPOLLIN: 表示对应的文件描述符可以读,包括对端SOCKET正常关闭
    // EPOLLONESHOT: 只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里 
    // EPOLLRDHUP: 表示对应的文件描述符被挂断
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.fd = fd;

    ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_MOD, fd, &ev);  // EPOLL_CTL_MOD 修改已经注册的fd的监听事件
    if (ret < 0) // 如果fd并没有被注册过
        ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, fd, &ev);  // EPOLL_CTL_ADD 注册新的fd到epoll实例中
    assert(ret != -1);
}

// 将fd注册到epoll实例中（如果还没有的话），fd的感兴趣事件为三个写相关的事件，参见_lthread_poller_ev_register_rd注释
inline void
_lthread_poller_ev_register_wr(int fd)
{
    struct epoll_event ev;
    int ret = 0;
    struct lthread_sched *sched = lthread_get_sched();

    ev.events = EPOLLOUT | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.fd = fd;
    ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_MOD, fd, &ev);
    if (ret < 0)
        ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, fd, &ev);
    assert(ret != -1);
}

// 返回和该就绪事件相关的文件描述符，例子：轮询发现某个文件描述符有数据了，这是一个事件，被发现有数据的这个文件描述符就是ev->data.fd
inline int
_lthread_poller_ev_get_fd(struct epoll_event *ev)
{
    return (ev->data.fd);
}

inline int
_lthread_poller_ev_get_event(struct epoll_event *ev)
{
    return (ev->events);
}

// 事件为：对应的文件描述符被挂断
inline int
_lthread_poller_ev_is_eof(struct epoll_event *ev)
{
    return (ev->events & EPOLLHUP);
}

// 事件为：对应的文件描述符可以写
inline int
_lthread_poller_ev_is_write(struct epoll_event *ev)
{
    return (ev->events & EPOLLOUT);
}

// 事件为：对应的文件描述符可以读
inline int
_lthread_poller_ev_is_read(struct epoll_event *ev)
{
    return (ev->events & EPOLLIN);
}

// 注册调度器自身的一个触发器，方式是创建一个eventfd，并将其放入epoll实例中，监听的目标事件为fd可读
inline void
_lthread_poller_ev_register_trigger(void)
{
    struct lthread_sched *sched = lthread_get_sched();
    int ret = 0;
    struct epoll_event ev;

    if (!sched->eventfd) {
        sched->eventfd = eventfd(0, EFD_NONBLOCK);  // 注意：触发器的fd是非阻塞 
        assert(sched->eventfd != -1);
    }
    ev.events = EPOLLIN;
    ev.data.fd = sched->eventfd;
    ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, sched->eventfd, &ev);
    assert(ret != -1);
}

// 清除触发器：方式是对eventfd进行一次读操作
inline void
_lthread_poller_ev_clear_trigger(void)
{
    uint64_t tmp;
    struct lthread_sched *sched = lthread_get_sched();
    assert(read(sched->eventfd, &tmp, sizeof(uint64_t)) == sizeof(uint64_t));   
}

// 对eventfd进行一次写操作
inline void
_lthread_poller_ev_trigger(struct lthread_sched *sched)
{
    uint64_t tmp = 2;
    assert(write(sched->eventfd, &tmp, sizeof(uint64_t)) == sizeof(uint64_t));
}
