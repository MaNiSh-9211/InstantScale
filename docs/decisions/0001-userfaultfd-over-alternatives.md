# 0001: Intercept memory absence with userfaultfd + epoll daemon

## Status: Accepted

## Context
Lazy migration needs a mechanism to (a) pause a thread exactly when it touches
absent memory and (b) resume it when the page arrives. Candidates: SIGSEGV +
signal-handler paging (fragile, global state), ptrace single-stepping
(syscall-heavy, breaks under Yama/seccomp), shattering pages via mprotect+
SIGBUS (coarse), or `userfaultfd(2)`.

## Decision
Register regions with `UFFDIO_REGISTER_MODE_MISSING` on a non-blocking
`userfaultfd`; a dedicated daemon thread integrates the uffd into an epoll
loop alongside sockets/control fds; installation uses `UFFDIO_COPY`
(ranged where profitable). Teardown is eventfd-driven (ADR-0006).

## Consequences
Easier: kernel parks/wakes threads with zero signal machinery; events queue
reliably; batching and merging are natural ioctl extensions. Harder: requires
kernel ≥ 4.11 features in practice (5.11+ for unprivileged default-on),
and container runtimes must permit the syscall (privileged containers or
sysctl) — documented in README/DOCKER.
