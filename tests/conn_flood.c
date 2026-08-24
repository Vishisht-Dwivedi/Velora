// conn_flood.c
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 4096

static volatile sig_atomic_t stop = 0;

typedef struct {
    int fd;
    uint8_t state;
} client_t;

enum {
    CONNECTING = 1,
    ESTABLISHED = 2
};

static void sig_handler(int sig)
{
    (void)sig;
    stop = 1;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1)
        return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1e9;
}

static int raise_fd_limit(void)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
        return -1;

    printf("[FD] soft=%lu hard=%lu\n",
           (unsigned long)rl.rlim_cur,
           (unsigned long)rl.rlim_max);

    if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;

        if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
            printf("[FD] raised soft limit to %lu\n",
                   (unsigned long)rl.rlim_cur);
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port> [target_connections]\n",
            argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);

    long target = 100000;

    if (argc == 4)
        target = atol(argv[3]);

    if (target <= 0) {
        fprintf(stderr, "Invalid target\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    raise_fd_limit();

    /*
     * Don't allocate a giant amount of state if the target is huge.
     * One small struct per connection is enough.
     */
    client_t *clients =
        calloc((size_t)target, sizeof(client_t));

    if (!clients) {
        perror("calloc clients");
        return 1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);

    if (epfd == -1) {
        perror("epoll_create1");
        free(clients);
        return 1;
    }

    struct sockaddr_in server = {0};

    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address\n");
        close(epfd);
        free(clients);
        return 1;
    }

    struct epoll_event *events =
        calloc(MAX_EVENTS, sizeof(*events));

    if (!events) {
        perror("calloc events");
        close(epfd);
        free(clients);
        return 1;
    }

    long connecting = 0;
    long established = 0;
    long failed = 0;

    long created = 0;

    double start = now_sec();
    double last_report = start;

    printf("\n");
    printf("========================================\n");
    printf(" Velora Connection Flood Client\n");
    printf("========================================\n");
    printf("Target : %s:%u\n", server_ip, port);
    printf("Target connections : %ld\n\n", target);

    /*
     * Main connection creation loop.
     *
     * We intentionally keep many connects outstanding.
     */
    while (!stop && established < target) {

        /*
         * Keep filling the connection pipeline.
         *
         * connect() on a nonblocking socket:
         *
         *   0          -> immediately connected
         *   EINPROGRESS -> connection in progress
         *   error      -> failed
         */
        while (!stop &&
               created < target &&
               (created - established) < 8192) {

            int fd = socket(
                AF_INET,
                SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                0
            );

            if (fd == -1) {
                if (errno == EMFILE || errno == ENFILE) {
                    fprintf(stderr,
                        "\n[FD LIMIT] errno=%d\n",
                        errno);
                    stop = 1;
                    break;
                }

                perror("socket");
                failed++;
                continue;
            }

            int ret = connect(
                fd,
                (struct sockaddr *)&server,
                sizeof(server)
            );

            if (ret == 0) {

                clients[created].fd = fd;
                clients[created].state = ESTABLISHED;

                established++;
                created++;

                struct epoll_event ev = {0};

                /*
                 * Keep the socket registered so we can detect
                 * connection failure later.
                 */
                ev.events = EPOLLIN | EPOLLERR |
                            EPOLLHUP | EPOLLRDHUP;

                ev.data.u32 = (uint32_t)(created - 1);

                if (epoll_ctl(
                        epfd,
                        EPOLL_CTL_ADD,
                        fd,
                        &ev) == -1) {

                    close(fd);
                    established--;
                    failed++;
                }

                continue;
            }

            if (errno != EINPROGRESS) {
                close(fd);
                failed++;
                created++;
                continue;
            }

            clients[created].fd = fd;
            clients[created].state = CONNECTING;

            struct epoll_event ev = {0};

            ev.events = EPOLLOUT |
                        EPOLLERR |
                        EPOLLHUP |
                        EPOLLRDHUP;

            ev.data.u32 = (uint32_t)created;

            if (epoll_ctl(
                    epfd,
                    EPOLL_CTL_ADD,
                    fd,
                    &ev) == -1) {

                close(fd);
                failed++;
            } else {
                connecting++;
            }

            created++;
        }

        /*
         * Process connection completions.
         */
        int n = epoll_wait(
            epfd,
            events,
            MAX_EVENTS,
            10
        );

        if (n == -1) {
            if (errno == EINTR)
                continue;

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {

            uint32_t idx = events[i].data.u32;

            if (idx >= (uint32_t)created)
                continue;

            client_t *c = &clients[idx];

            if (c->state != CONNECTING)
                continue;

            int err = 0;
            socklen_t len = sizeof(err);

            if (getsockopt(
                    c->fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &err,
                    &len) == -1) {

                err = errno;
            }

            if (err == 0) {

                c->state = ESTABLISHED;

                connecting--;
                established++;

                /*
                 * We no longer need EPOLLOUT.
                 *
                 * Keep the connection registered for errors/HUP.
                 */
                struct epoll_event ev = {0};

                ev.events =
                    EPOLLIN |
                    EPOLLERR |
                    EPOLLHUP |
                    EPOLLRDHUP;

                ev.data.u32 = idx;

                epoll_ctl(
                    epfd,
                    EPOLL_CTL_MOD,
                    c->fd,
                    &ev
                );

            } else {

                close(c->fd);
                c->fd = -1;
                c->state = 0;

                connecting--;
                failed++;
            }
        }

        /*
         * Periodic statistics.
         */
        double now = now_sec();

        if (now - last_report >= 1.0) {

            printf(
                "\rcreated=%ld  connecting=%ld  "
                "established=%ld  failed=%ld",
                created,
                connecting,
                established,
                failed
            );

            fflush(stdout);
            last_report = now;
        }
    }

    printf("\n\n");

    double elapsed = now_sec() - start;

    printf("========================================\n");
    printf(" Connection benchmark complete\n");
    printf("========================================\n");
    printf("Created      : %ld\n", created);
    printf("Established  : %ld\n", established);
    printf("Connecting   : %ld\n", connecting);
    printf("Failed       : %ld\n", failed);
    printf("Time         : %.3f s\n", elapsed);

    if (elapsed > 0)
        printf("Rate         : %.0f connections/s\n",
               (double)established / elapsed);

    printf("\nHolding %ld connections open.\n",
           established);
    printf("Press Ctrl-C to close them.\n");

    /*
     * IMPORTANT:
     *
     * Don't immediately close the sockets.
     *
     * We want Velora to actually have to maintain all of
     * these connections simultaneously.
     */
    while (!stop) {
        sleep(1);
    }

    printf("\nClosing connections...\n");

    for (long i = 0; i < created; i++) {
        if (clients[i].fd >= 0)
            close(clients[i].fd);
    }

    free(events);
    close(epfd);
    free(clients);

    printf("Done.\n");

    return 0;
}