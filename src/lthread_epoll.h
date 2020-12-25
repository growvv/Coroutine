#ifndef LTHREAD_EPOLL_H_
#define LTHREAD_EPOLL_H_

#include "lthread_int.h"
#include <assert.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

int _lthread_poller_create(void);
int _lthread_poller_poll(struct timespec t);
void _lthread_poller_ev_clear_rd(int fd);
void _lthread_poller_ev_clear_wr(int fd);
void _lthread_poller_ev_register_rd(int fd);
void _lthread_poller_ev_register_wr(int fd);
int _lthread_poller_ev_get_fd(struct epoll_event *ev);
int _lthread_poller_ev_get_event(struct epoll_event *ev);
int _lthread_poller_ev_is_eof(struct epoll_event *ev);
int _lthread_poller_ev_is_write(struct epoll_event *ev);
int _lthread_poller_ev_is_read(struct epoll_event *ev);
void _lthread_poller_ev_register_trigger(void);
void _lthread_poller_ev_clear_trigger(void);
void _lthread_poller_ev_trigger(struct lthread_sched *sched);

#endif