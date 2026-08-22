/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_EVENT_H
#define KURONO_EVENT_H

/*
 * event.c is a pure demultiplexer over epoll (DESIGN.md III.1): it owns
 * the fds and reports which source fired. It never dispatches -- main.c
 * holds every dispatch target. This header pulls in no epoll/signalfd/
 * timerfd types on purpose, so nothing outside event.c needs to know how
 * a source is implemented.
 */

typedef enum {
  EVSRC_NONE = 0,
  EVSRC_TTY,
  EVSRC_RESIZE,
  EVSRC_SIGNAL,
  EVSRC_PIPE,
  EVSRC_REDRAW,
  EVSRC_RECONNECT,
  EVSRC_TTY_HUP,
  EVSRC_COUNT
} EventSource;

typedef struct EventLoop EventLoop;

/*
 * event_create: build the epoll fd, the signalfd for SIGINT/SIGTERM/
 * SIGHUP (blocking that set in the calling thread first, via
 * pthread_sigmask, before any other thread can exist -- DESIGN.md
 * IV.9.a step 2), the non-blocking self-pipe, and both timerfds
 * (created disarmed). Registers the signalfd, the pipe read end, and
 * both timerfds with epoll. Takes nothing, returns a heap-allocated
 * EventLoop on success or NULL on failure -- on failure every fd
 * opened so far has already been closed.
 */
EventLoop * event_create(void);

/*
 * event_destroy: close every fd this EventLoop owns and free it. Safe
 * to call on a partially-constructed EventLoop (every fd not yet
 * opened is left at its sentinel and skipped). Takes the EventLoop,
 * returns nothing. el may be NULL.
 */
void event_destroy(EventLoop * el);

/*
 * event_add: register an externally-owned fd (the termbox tty fd or
 * resize fd, from phase 11) with epoll under the given EventSource
 * tag. event.c does not open, read, or close this fd. Takes the
 * EventLoop, the tag to report when it fires, and the fd. Returns 0
 * on success, -1 on failure.
 */
int event_add(EventLoop * el, EventSource src, int fd);

/*
 * event_wait: block in epoll_wait (retrying on EINTR) for up to
 * timeout_ms (-1 blocks indefinitely), then fill out[] with the
 * EventSource tag of every source that fired, up to max entries. A
 * fired redraw_timerfd or reconnect_timerfd has its expiration counter
 * read() here so epoll does not immediately re-fire (both are
 * level-triggered). This function only reports; it never dispatches.
 * Takes the EventLoop, the output array, its capacity, and the
 * timeout. Returns the number of entries written to out[], or -1 on
 * error.
 */
int event_wait(EventLoop * el, EventSource * out, int max, int timeout_ms);

/*
 * event_signalfd: the raw signalfd, for main.c to read a
 * signalfd_siginfo from if it wants to know which of SIGINT/SIGTERM/
 * SIGHUP fired. Takes the EventLoop, returns the fd.
 */
int event_signalfd(const EventLoop * el);

/*
 * event_pipe_read_fd: the self-pipe's read end, for main.c to drain
 * after an EVSRC_PIPE wakeup. Takes the EventLoop, returns the fd.
 */
int event_pipe_read_fd(const EventLoop * el);

/*
 * event_pipe_write_fd: the self-pipe's write end, O_NONBLOCK. This is
 * the fd the audio thread's notification callback (phase 5) writes a
 * single byte to; the write must never block, which is why the pipe
 * was created O_NONBLOCK. Takes the EventLoop, returns the fd.
 */
int event_pipe_write_fd(const EventLoop * el);

/*
 * event_arm_redraw: arm redraw_timerfd to fire hz times per second,
 * or disarm it if hz == 0. Takes the EventLoop and the rate. Returns
 * 0 on success, -1 on failure.
 */
int event_arm_redraw(EventLoop * el, int hz);

/*
 * event_arm_reconnect: arm reconnect_timerfd as a one-shot firing
 * after ms milliseconds, or disarm it if ms == 0. Takes the EventLoop
 * and the delay. Returns 0 on success, -1 on failure.
 */
int event_arm_reconnect(EventLoop * el, int ms);

#endif // KURONO_EVENT_H
