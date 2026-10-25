/* Test-only fault injection for the first accepted control client. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int target = -1, chosen, receives, sends;

static int mode(const char *name) {
    const char *value = getenv("ZERO_TEST_CONTROL_IO");
    return value && strcmp(value, name) == 0;
}

static ssize_t fail(int error, const char *message) {
    ssize_t written = write(STDERR_FILENO, message, strlen(message));
    (void)written;
    errno = error;
    return -1;
}

int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    int client = syscall(SYS_accept4, fd, address, length, flags);
    if (client >= 0 && !chosen) {
        target = client;
        chosen = 1;
    }
    return client;
}

ssize_t recv(int fd, void *buffer, size_t size, int flags) {
    if (fd == target) {
        if (mode("recv-error")) return fail(ECONNRESET, "injected recv reset\n");
        if (mode("retry") && receives++ == 0)
            return fail(EINTR, "injected recv EINTR\n");
    }
    return syscall(SYS_recvfrom, fd, buffer, size, flags, NULL, NULL);
}

ssize_t send(int fd, const void *buffer, size_t size, int flags) {
    if (fd == target) {
        if (mode("send-error")) return fail(EPIPE, "injected send EPIPE\n");
        if (mode("blocked-send")) {
            if (sends++ == 0) return fail(EAGAIN, "injected send blocked\n");
            errno = EAGAIN;
            return -1;
        }
        if (mode("retry")) {
            if (sends++ == 0) return fail(EAGAIN, "injected send EAGAIN\n");
            if (sends == 2) return fail(EINTR, "injected send EINTR\n");
        }
    }
    return syscall(SYS_sendto, fd, buffer, size, flags, NULL, 0);
}

int close(int fd) {
    if (fd == target) target = -1; /* Do not inject into a later reused fd. */
    return syscall(SYS_close, fd);
}
