# 0006: Teardown via stop-eventfd poked before pthread_join

## Status: Accepted

## Context
Phase 1 originally tore down by closing the userfaultfd from the main thread;
the daemon's epoll then reported EPOLLHUP. Under load, a race appeared where
the daemon drained its queue, re-entered `read(uffd_msg)` after main had
closed the fd, and died on EBADF (`read(uffd_msg) failed: errno=9`) before
reaching HUP handling.

## Decision
The daemon owns an `eventfd` registered in the same epoll set. Main writes
`1` to it, joins the thread, and only then closes uffd/sock/eventfd.
Nobody ever touches an fd another thread may be using.

## Consequences
Easier: deterministic shutdown, no signal tricks, works for any number of
monitored fds. Harder: one extra fd per daemon; the ordering rule
("poke → join → close") must be preserved by future contributors.
