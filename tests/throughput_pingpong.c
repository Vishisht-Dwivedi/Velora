#define _POSIX_C_SOURCE 200809L

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
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define VR_MAGIC 0x5789
#define VR_VERSION 1

#define VR_PKT_CONNECT 1
#define VR_PKT_CONNECT_ACK 2
#define VR_PKT_PING 3
#define VR_PKT_PONG 4

#define HEADER_SIZE 8

#define DEFAULT_CLIENTS 10000
#define DEFAULT_PIPELINE 16
#define DEFAULT_DURATION 30

#define MAX_EVENTS 1024
#define RX_BUFFER_SIZE 4096

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1000000000.0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1)
        return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Velora packet:
 *
 * +--------+---------+------+----------+-------+-------------+
 * | magic  | version | type | stream   | flags | payload_len |
 * | 2 B    | 1 B     | 1 B  | 1 B      | 1 B   | 2 B         |
 * +--------+---------+------+----------+-------+-------------+
 */
static void build_packet(
    uint8_t *packet,
    uint8_t type
)
{
    packet[0] = (uint8_t)(VR_MAGIC >> 8);
    packet[1] = (uint8_t)(VR_MAGIC & 0xff);

    packet[2] = VR_VERSION;
    packet[3] = type;
    packet[4] = 0; /* stream_id */
    packet[5] = 0; /* flags */
    packet[6] = 0; /* payload_len high */
    packet[7] = 0; /* payload_len low */
}

static int recv_exact_blocking(
    int fd,
    uint8_t *buf,
    size_t len
)
{
    size_t received = 0;

    while (received < len)
    {
        ssize_t n = recv(
            fd,
            buf + received,
            len - received,
            0
        );

        if (n > 0)
        {
            received += (size_t)n;
            continue;
        }

        if (n == 0)
            return -1;

        if (errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

static int send_exact_blocking(
    int fd,
    const uint8_t *buf,
    size_t len
)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(
            fd,
            buf + sent,
            len - sent,
            MSG_NOSIGNAL
        );

        if (n > 0)
        {
            sent += (size_t)n;
            continue;
        }

        if (n == -1 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

typedef struct
{
    int fd;

    /*
     * Number of PING packets currently outstanding on this connection.
     * Every PONG received decrements it.
     *
     * We keep the pipeline bounded so the benchmark exercises the
     * complete request/response path without creating an unbounded
     * amount of kernel buffering.
     */
    uint32_t outstanding;

    /*
     * RX state.
     *
     * PONGs have a fixed 8-byte packet size, but TCP can split or
     * coalesce them arbitrarily.
     */
    uint8_t rx[RX_BUFFER_SIZE];
    size_t rx_len;

    uint64_t completed;
    uint64_t errors;

    int alive;

} client_t;

static int establish_connection(
    const char *host,
    uint16_t port,
    client_t *client
)
{
    struct sockaddr_in addr;
    uint8_t connect_packet[HEADER_SIZE];
    uint8_t response[HEADER_SIZE];

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        fprintf(stderr, "[ERROR] Invalid IPv4 address: %s\n", host);
        return -1;
    }

    int fd = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );

    if (fd == -1)
        return -1;

    /*
     * Connection setup is deliberately blocking.
     * It is excluded from the benchmark timing.
     */
    if (connect(
            fd,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) == -1)
    {
        close(fd);
        return -1;
    }

    build_packet(connect_packet, VR_PKT_CONNECT);

    if (send_exact_blocking(
            fd,
            connect_packet,
            sizeof(connect_packet)
        ) == -1)
    {
        close(fd);
        return -1;
    }

    if (recv_exact_blocking(
            fd,
            response,
            sizeof(response)
        ) == -1)
    {
        close(fd);
        return -1;
    }

    uint16_t magic =
        ((uint16_t)response[0] << 8) |
        response[1];

    if (magic != VR_MAGIC ||
        response[2] != VR_VERSION ||
        response[3] != VR_PKT_CONNECT_ACK ||
        response[4] != 0 ||
        response[5] != 0 ||
        response[6] != 0 ||
        response[7] != 0)
    {
        fprintf(
            stderr,
            "[CONNECT FAIL] invalid CONNECT_ACK "
            "(magic=0x%04x version=%u type=%u)\n",
            magic,
            response[2],
            response[3]
        );

        close(fd);
        return -1;
    }

    if (set_nonblocking(fd) == -1)
    {
        close(fd);
        return -1;
    }

    memset(client, 0, sizeof(*client));

    client->fd = fd;
    client->alive = 1;

    return 0;
}

static int register_client(
    int epfd,
    client_t *client,
    uint32_t index
)
{
    struct epoll_event ev = {0};

    /*
     * We need both directions:
     *
     * EPOLLOUT -> issue more PINGs
     * EPOLLIN  -> collect PONGs
     */
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.u32 = index;

    return epoll_ctl(
        epfd,
        EPOLL_CTL_ADD,
        client->fd,
        &ev
    );
}

/*
 * Send as many PINGs as needed to fill the connection's pipeline.
 */
static int fill_pipeline(
    client_t *client,
    uint32_t pipeline,
    uint64_t *send_failures
)
{
    static const uint8_t ping_packet[HEADER_SIZE] = {
        (uint8_t)(VR_MAGIC >> 8),
        (uint8_t)(VR_MAGIC & 0xff),
        VR_VERSION,
        VR_PKT_PING,
        0,
        0,
        0,
        0
    };

    while (
        client->alive &&
        client->outstanding < pipeline
    )
    {
        ssize_t n = send(
            client->fd,
            ping_packet,
            sizeof(ping_packet),
            MSG_NOSIGNAL
        );

        if (n == (ssize_t)sizeof(ping_packet))
        {
            client->outstanding++;
            continue;
        }

        if (n == -1 &&
            (errno == EAGAIN ||
             errno == EWOULDBLOCK))
        {
            /*
             * Socket cannot accept more right now.
             * EPOLLOUT will wake us again.
             */
            return 0;
        }

        if (n == -1 && errno == EINTR)
            continue;

        client->errors++;
        (*send_failures)++;

        return -1;
    }

    return 0;
}

/*
 * Parse as many complete PONG packets as are available.
 *
 * Since PONG has an 8-byte zero-payload envelope, every complete
 * response is exactly 8 bytes.
 */
static uint64_t process_pongs(
    client_t *client
)
{
    uint64_t completed = 0;

    while (client->rx_len >= HEADER_SIZE)
    {
        uint8_t *packet = client->rx;

        uint16_t magic =
            ((uint16_t)packet[0] << 8) |
            packet[1];

        uint8_t version = packet[2];
        uint8_t type = packet[3];
        uint8_t stream_id = packet[4];
        uint8_t flags = packet[5];

        uint16_t payload_len =
            ((uint16_t)packet[6] << 8) |
            packet[7];

        /*
         * A valid PONG for this benchmark must be exactly:
         *
         * magic      = 0x5789
         * version    = 1
         * type       = PONG
         * stream_id  = 0
         * flags      = 0
         * payload    = 0
         */
        if (magic != VR_MAGIC ||
            version != VR_VERSION ||
            type != VR_PKT_PONG ||
            stream_id != 0 ||
            flags != 0 ||
            payload_len != 0)
        {
            fprintf(
                stderr,
                "[PROTOCOL FAIL] fd=%d "
                "magic=0x%04x version=%u type=%u "
                "stream=%u flags=%u payload=%u\n",
                client->fd,
                magic,
                version,
                type,
                stream_id,
                flags,
                payload_len
            );

            client->errors++;
            return completed;
        }

        /*
         * Remove one complete PONG from RX.
         */
        memmove(
            client->rx,
            client->rx + HEADER_SIZE,
            client->rx_len - HEADER_SIZE
        );

        client->rx_len -= HEADER_SIZE;

        if (client->outstanding == 0)
        {
            /*
             * Should never happen if the benchmark logic is correct.
             */
            client->errors++;
            continue;
        }

        client->outstanding--;
        client->completed++;
        completed++;
    }

    return completed;
}

static int drain_reads(
    client_t *client,
    uint64_t *completed
)
{
    while (client->alive)
    {
        if (client->rx_len == sizeof(client->rx))
        {
            fprintf(
                stderr,
                "[ERROR] RX buffer full on fd=%d\n",
                client->fd
            );

            client->errors++;
            return -1;
        }

        ssize_t n = recv(
            client->fd,
            client->rx + client->rx_len,
            sizeof(client->rx) - client->rx_len,
            0
        );

        if (n > 0)
        {
            client->rx_len += (size_t)n;

            *completed += process_pongs(client);

            continue;
        }

        if (n == 0)
        {
            client->alive = 0;
            return -1;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN ||
            errno == EWOULDBLOCK)
        {
            return 0;
        }

        client->errors++;
        return -1;
    }

    return -1;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    uint16_t port = 22409;

    int client_count = DEFAULT_CLIENTS;
    uint32_t pipeline = DEFAULT_PIPELINE;
    int duration = DEFAULT_DURATION;

    if (argc > 1)
        host = argv[1];

    if (argc > 2)
        port = (uint16_t)strtoul(argv[2], NULL, 10);

    if (argc > 3)
        client_count = atoi(argv[3]);

    if (argc > 4)
        pipeline = (uint32_t)strtoul(argv[4], NULL, 10);

    if (argc > 5)
        duration = atoi(argv[5]);

    if (client_count <= 0 ||
        pipeline == 0 ||
        duration <= 0)
    {
        fprintf(
            stderr,
            "Usage: %s [host] [port] [connections] [pipeline] [duration]\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("\n");
    printf("========================================================================\n");
    printf(" VELORA BLACK-BOX PING/PONG THROUGHPUT TEST\n");
    printf("========================================================================\n\n");

    printf("[CONFIG] host              : %s\n", host);
    printf("[CONFIG] port              : %u\n", port);
    printf("[CONFIG] connections       : %d\n", client_count);
    printf("[CONFIG] pipeline/conn     : %u PINGs\n", pipeline);
    printf("[CONFIG] packet size       : 8 bytes\n");
    printf("[CONFIG] duration          : %d seconds\n", duration);

    printf("\n[MEASUREMENT]\n");
    printf("  A message is counted ONLY when a valid PONG is received.\n");
    printf("  TCP send acceptance is NOT counted as throughput.\n");
    printf("  Connection establishment is excluded from timing.\n");
    printf("  Velora is treated as a black box.\n\n");

    client_t *clients =
        calloc((size_t)client_count, sizeof(client_t));

    if (clients == NULL)
    {
        perror("calloc clients");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < client_count; i++)
        clients[i].fd = -1;

    /*
     * -------------------------------------------------------------
     * Connection setup
     * -------------------------------------------------------------
     */

    printf(
        "[SETUP] Establishing %d connections...\n",
        client_count
    );

    int established = 0;

    for (int i = 0; i < client_count; i++)
    {
        if (establish_connection(
                host,
                port,
                &clients[i]
            ) == -1)
        {
            fprintf(
                stderr,
                "[CONNECT FAIL] client=%d\n",
                i
            );

            continue;
        }

        established++;

        if (established % 1000 == 0 ||
            established == client_count)
        {
            printf(
                "[CONNECT] %d/%d established\n",
                established,
                client_count
            );

            fflush(stdout);
        }
    }

    printf("\n");
    printf("[SETUP COMPLETE]\n");
    printf(
        "  Requested connections : %d\n",
        client_count
    );

    printf(
        "  Established           : %d\n",
        established
    );

    if (established == 0)
    {
        fprintf(stderr, "[FATAL] No connections established.\n");

        free(clients);
        return EXIT_FAILURE;
    }

    /*
     * -------------------------------------------------------------
     * Client-side epoll
     * -------------------------------------------------------------
     */

    int epfd = epoll_create1(EPOLL_CLOEXEC);

    if (epfd == -1)
    {
        perror("epoll_create1");

        for (int i = 0; i < client_count; i++)
        {
            if (clients[i].fd >= 0)
                close(clients[i].fd);
        }

        free(clients);

        return EXIT_FAILURE;
    }

    for (int i = 0; i < client_count; i++)
    {
        if (!clients[i].alive)
            continue;

        if (register_client(
                epfd,
                &clients[i],
                (uint32_t)i
            ) == -1)
        {
            perror("epoll_ctl ADD");

            clients[i].alive = 0;
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    }

    struct epoll_event events[MAX_EVENTS];

    printf("\n[BENCHMARK] Starting...\n");
    printf("[BENCHMARK] Setup time excluded.\n\n");

    /*
     * -------------------------------------------------------------
     * Measurement begins HERE.
     * -------------------------------------------------------------
     */

    double start = monotonic_seconds();
    double last_report = start;

    uint64_t total_completed = 0;
    uint64_t total_errors = 0;

    int active = established;

    while (!stop_requested)
    {
        double now = monotonic_seconds();

        if (now - start >= (double)duration)
            break;

        int ready = epoll_wait(
            epfd,
            events,
            MAX_EVENTS,
            10
        );

        if (ready == -1)
        {
            if (errno == EINTR)
                continue;

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < ready; i++)
        {
            uint32_t index = events[i].data.u32;

            if (index >= (uint32_t)client_count)
                continue;

            client_t *client = &clients[index];

            if (!client->alive)
                continue;

            /*
             * Read first when both directions are ready.
             *
             * This prevents PONGs from accumulating while we
             * immediately refill the pipeline.
             */
            if (events[i].events & EPOLLIN)
            {
                uint64_t completed = 0;

                if (drain_reads(
                        client,
                        &completed
                    ) == -1)
                {
                    if (client->alive)
                    {
                        client->alive = 0;
                        close(client->fd);
                        client->fd = -1;
                    }

                    active--;
                    continue;
                }

                total_completed += completed;
            }

            /*
             * Refill the pipeline after receiving responses.
             */
            if (client->alive &&
                (events[i].events & EPOLLOUT))
            {
                uint64_t before =
                    client->errors;

                if (fill_pipeline(
                        client,
                        pipeline,
                        &total_errors
                    ) == -1)
                {
                    client->errors +=
                        (client->errors == before);

                    client->alive = 0;

                    epoll_ctl(
                        epfd,
                        EPOLL_CTL_DEL,
                        client->fd,
                        NULL
                    );

                    close(client->fd);
                    client->fd = -1;

                    active--;
                }
            }
        }

        /*
         * For sockets that did not happen to emit EPOLLOUT in this
         * exact iteration, fill any empty pipeline directly.
         */
        for (int i = 0; i < client_count; i++)
        {
            client_t *client = &clients[i];

            if (!client->alive)
                continue;

            if (client->outstanding >= pipeline)
                continue;

            if (fill_pipeline(
                    client,
                    pipeline,
                    &total_errors
                ) == -1)
            {
                client->alive = 0;

                epoll_ctl(
                    epfd,
                    EPOLL_CTL_DEL,
                    client->fd,
                    NULL
                );

                close(client->fd);
                client->fd = -1;

                active--;
            }
        }

        /*
         * Periodic client-side telemetry.
         */
        now = monotonic_seconds();

        if (now - last_report >= 1.0)
        {
            double elapsed = now - start;
            double rate =
                elapsed > 0.0
                    ? (double)total_completed / elapsed
                    : 0.0;

            uint64_t current_errors = total_errors;

            for (int i = 0; i < client_count; i++)
                current_errors += clients[i].errors;

            printf(
                "[%7.2fs] "
                "active=%5d "
                "completed=%12llu "
                "throughput=%10.0f PONG/s "
                "errors=%llu\n",

                elapsed,
                active,

                (unsigned long long)
                    total_completed,

                rate,

                (unsigned long long)
                    current_errors
            );

            fflush(stdout);

            last_report = now;
        }

        if (active == 0)
        {
            fprintf(
                stderr,
                "[FATAL] All connections failed.\n"
            );

            break;
        }
    }

    /*
     * -------------------------------------------------------------
     * Final measurement
     * -------------------------------------------------------------
     */

    double end = monotonic_seconds();
    double elapsed = end - start;

    total_errors = 0;

    uint64_t total_client_completed = 0;

    for (int i = 0; i < client_count; i++)
    {
        total_client_completed +=
            clients[i].completed;

        total_errors +=
            clients[i].errors;
    }

    /*
     * total_completed already counts all PONGs encountered.
     * Use the client totals as a sanity check.
     */
    if (total_client_completed != total_completed)
    {
        fprintf(
            stderr,
            "[WARNING] Counter mismatch: "
            "aggregate=%llu per-client=%llu\n",
            (unsigned long long)total_completed,
            (unsigned long long)total_client_completed
        );
    }

    double throughput =
        elapsed > 0.0
            ? (double)total_completed / elapsed
            : 0.0;

    printf("\n");
    printf("========================================================================\n");
    printf(" FINAL RESULTS\n");
    printf("========================================================================\n\n");

    printf(
        "  Benchmark runtime       : %.2f s\n",
        elapsed
    );

    printf(
        "  Requested connections   : %d\n",
        client_count
    );

    printf(
        "  Connections alive       : %d\n",
        active
    );

    printf(
        "  Pipeline depth          : %u PINGs/connection\n",
        pipeline
    );

    printf("\n");

    printf(
        "  Completed PING/PONGs    : %llu\n",
        (unsigned long long)total_completed
    );

    printf(
        "  Average throughput      : %.0f completed msg/s\n",
        throughput
    );

    printf(
        "  Total errors            : %llu\n",
        (unsigned long long)total_errors
    );

    printf(
        "  Average completions/conn: %.0f\n",
        established > 0
            ? (double)total_completed / established
            : 0.0
    );

    printf("\n");

    if (total_errors == 0 &&
        active == established)
    {
        printf(
            "[RESULT] PASS - "
            "all connections remained healthy\n"
        );
    }
    else
    {
        printf(
            "[RESULT] WARNING - "
            "errors or connection loss detected\n"
        );
    }

    printf(
        "\n[INTERPRETATION]\n"
        "  Throughput = valid PONGs received / measured runtime.\n"
        "  This is an application-level black-box measurement.\n"
    );

    printf("========================================================================\n");

    /*
     * Cleanup.
     */
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].fd >= 0)
            close(clients[i].fd);
    }

    close(epfd);
    free(clients);

    return EXIT_SUCCESS;
}