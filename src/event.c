/*
 * pipe2() is a GNU extension declared in <unistd.h> only under
 * __USE_GNU; the Makefile builds with -D_POSIX_C_SOURCE=200809L, which
 * does not set it. signalfd/timerfd/epoll are unconditionally declared
 * by their own Linux headers and need nothing extra. Scoped to this
 * translation unit only -- it does not loosen any other file's build.
 */
#define _GNU_SOURCE

#include "../include/event.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* epoll_wait's own event buffer, stack-allocated: bounded by the fixed
   number of sources this loop can ever register, not by the caller's
   `max`. No malloc, no VLA. */
#define EVENT_MAX_EPOLL_EVENTS 8

/*
 * EventLoop: every fd it owns. Sentinel value -1 marks "not yet
 * opened", which is what makes event_destroy safe to call on a
 * partially-constructed instance -- event_create zeroes-then-sentinels
 * every field before attempting to open anything.
 */
struct EventLoop {
  int epollfd;
  int sigfd;
  int pipe_fd[2];
  int redraw_timerfd;
  int reconnect_timerfd;
};

/*
 * epoll_add: epoll_ctl(ADD) helper, tagging the registered fd with the
 * EventSource it should report as. Takes the epoll fd, the fd to
 * register, and the tag. Returns 0 on success, -1 on failure.
 */
static int
epoll_add(int epollfd, int fd, EventSource src)
{
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.u32 = (uint32_t) src;
  return epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
} /* epoll_add */

/*
 * event_destroy: see include/event.h for the contract.
 */
void
event_destroy(EventLoop * el)
{
  if (el == NULL) {
    return;
  }

  if (el->reconnect_timerfd >= 0) {
    close(el->reconnect_timerfd);
  }
  if (el->redraw_timerfd >= 0) {
    close(el->redraw_timerfd);
  }
  if (el->pipe_fd[1] >= 0) {
    close(el->pipe_fd[1]);
  }
  if (el->pipe_fd[0] >= 0) {
    close(el->pipe_fd[0]);
  }
  if (el->sigfd >= 0) {
    close(el->sigfd);
  }
  if (el->epollfd >= 0) {
    close(el->epollfd);
  }

  free(el);
} /* event_destroy */

/*
 * event_create: see include/event.h for the contract.
 */
EventLoop *
event_create(void)
{
  EventLoop * el = malloc(sizeof(*el));
  if (el == NULL) {
    return NULL;
  }
  el->epollfd = -1;
  el->sigfd = -1;
  el->pipe_fd[0] = -1;
  el->pipe_fd[1] = -1;
  el->redraw_timerfd = -1;
  el->reconnect_timerfd = -1;

  /* DESIGN.md IV.9.a step 2: block SIGINT/SIGTERM/SIGHUP in this thread
     before any other thread can exist, so every later pthread_create
     (T2, and miniaudio's T3) inherits the mask and can never take one
     of them. The terminal resize signal is deliberately absent from
     this set and from this file entirely -- termbox2 owns it via its
     own handler and self-pipe (DESIGN.md VIII.2). Blocking it here
     would silently disable resize handling with no other symptom. */
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
    goto fail;
  }

  el->sigfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
  if (el->sigfd < 0) {
    goto fail;
  }

  /* O_NONBLOCK on the write end is load-bearing (phase 5): the audio
     thread's notification callback writes one byte to it and must
     never block. */
  if (pipe2(el->pipe_fd, O_NONBLOCK | O_CLOEXEC) != 0) {
    goto fail;
  }

  el->redraw_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (el->redraw_timerfd < 0) {
    goto fail;
  }

  /* Created and registered now even though the reopen ladder that
     drives it lands in phase 8 -- it belongs to the fd inventory
     event.c owns (plan.md phase-4-eventloop.md II). */
  el->reconnect_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (el->reconnect_timerfd < 0) {
    goto fail;
  }

  el->epollfd = epoll_create1(EPOLL_CLOEXEC);
  if (el->epollfd < 0) {
    goto fail;
  }

  if (epoll_add(el->epollfd, el->sigfd, EVSRC_SIGNAL) != 0 ||
      epoll_add(el->epollfd, el->pipe_fd[0], EVSRC_PIPE) != 0 ||
      epoll_add(el->epollfd, el->redraw_timerfd, EVSRC_REDRAW) != 0 ||
      epoll_add(el->epollfd, el->reconnect_timerfd, EVSRC_RECONNECT) != 0) {
    goto fail;
  }

  return el;

fail:
  event_destroy(el);
  return NULL;
} /* event_create */

/*
 * event_add: see include/event.h for the contract.
 */
int
event_add(EventLoop * el, EventSource src, int fd)
{
  return epoll_add(el->epollfd, fd, src);
} /* event_add */

/*
 * event_wait: see include/event.h for the contract.
 */
int
event_wait(EventLoop * el, EventSource * out, int max, int timeout_ms)
{
  struct epoll_event evs[EVENT_MAX_EPOLL_EVENTS];
  int cap = (max < EVENT_MAX_EPOLL_EVENTS) ? max : EVENT_MAX_EPOLL_EVENTS;
  if (cap <= 0) {
    return 0;
  }

  int n;
  for (;;) {
    n = epoll_wait(el->epollfd, evs, cap, timeout_ms);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    break;
  }

  int count = 0;
  for (int i = 0; i < n; i++) {
    EventSource src = (EventSource) evs[i].data.u32;

    /* epoll always reports HUP/ERR regardless of the requested event
       mask. On the tty specifically this must be reclassified so
       main.c can tell "hangup, shut down" from "input, read it" --
       DESIGN.md VIII.2's warning on the 100% CPU spin. */
    if (src == EVSRC_TTY && (evs[i].events & (EPOLLHUP | EPOLLERR)) != 0) {
      src = EVSRC_TTY_HUP;
    }

    /* Both timerfds are level-triggered: their expiration counter must
       be read() on every wakeup or epoll re-fires immediately. Neither
       fd has an accessor in event.h, so draining them is event.c's job. */
    if (src == EVSRC_REDRAW || src == EVSRC_RECONNECT) {
      uint64_t expirations;
      int tfd = (src == EVSRC_REDRAW) ? el->redraw_timerfd : el->reconnect_timerfd;
      ssize_t r = read(tfd, &expirations, sizeof(expirations));
      (void) r; /* nonblocking; a spurious EAGAIN here is not an error */
    }

    out[count++] = src;
  }

  return count;
} /* event_wait */

/*
 * event_signalfd: see include/event.h for the contract.
 */
int
event_signalfd(const EventLoop * el)
{
  return el->sigfd;
} /* event_signalfd */

/*
 * event_pipe_read_fd: see include/event.h for the contract.
 */
int
event_pipe_read_fd(const EventLoop * el)
{
  return el->pipe_fd[0];
} /* event_pipe_read_fd */

/*
 * event_pipe_write_fd: see include/event.h for the contract.
 */
int
event_pipe_write_fd(const EventLoop * el)
{
  return el->pipe_fd[1];
} /* event_pipe_write_fd */

/*
 * event_arm_redraw: see include/event.h for the contract.
 */
int
event_arm_redraw(EventLoop * el, int hz)
{
  struct itimerspec its;
  memset(&its, 0, sizeof(its));

  if (hz > 0) {
    long ns = 1000000000L / hz;
    its.it_value.tv_sec = ns / 1000000000L;
    its.it_value.tv_nsec = ns % 1000000000L;
    its.it_interval = its.it_value;
  }
  /* hz == 0: the zeroed itimerspec disarms the timer. */

  return timerfd_settime(el->redraw_timerfd, 0, &its, NULL);
} /* event_arm_redraw */

/*
 * event_arm_reconnect: see include/event.h for the contract.
 */
int
event_arm_reconnect(EventLoop * el, int ms)
{
  struct itimerspec its;
  memset(&its, 0, sizeof(its));

  if (ms > 0) {
    its.it_value.tv_sec = ms / 1000;
    its.it_value.tv_nsec = (long) (ms % 1000) * 1000000L;
    /* it_interval stays zero: one-shot. */
  }
  /* ms == 0: the zeroed itimerspec disarms the timer. */

  return timerfd_settime(el->reconnect_timerfd, 0, &its, NULL);
} /* event_arm_reconnect */
