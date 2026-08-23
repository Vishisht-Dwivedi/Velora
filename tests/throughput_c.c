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
#define VR_PKT_PUBLISH 8

#define HEADER_SIZE 8
#define DEFAULT_CLIENTS 10000
#define DEFAULT_PAYLOAD_SIZE 64
#define DEFAULT_BATCH_PACKETS 16
#define DEFAULT_DURATION 30

#define MAX_EVENTS 1024

typedef struct
{
    int fd;

    /*
     * Offset into batch.
     *
     * The batch contains complete back-to-back Velora packets.
     * A non-zero offset means the previous send() stopped part-way
     * through the batch.
     */
    size_t batch_offset;

    uint64_t messages;
    uint64_t bytes;
    uint64_t failures;

} client_t;

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1000000000.0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return -1;

    return 0;
}

static int send_all_blocking(int fd, const uint8_t *buf, size_t len)
{
    size_t sent_total = 0;

    while (sent_total < len)
    {
        ssize_t n = send(
            fd,
            buf + sent_total,
            len - sent_total,
            MSG_NOSIGNAL
        );

        if (n > 0)
        {
            sent_total += (size_t)n;
            continue;
        }

        if (n == -1 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

static int recv_exact(int fd, uint8_t *buf, size_t len)
{
    size_t received_total = 0;

    while (received_total < len)
    {
        ssize_t n = recv(
            fd,
            buf + received_total,
            len - received_total,
            0
        );

        if (n > 0)
        {
            received_total += (size_t)n;
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

/*
 * Velora header:
 *
 *   magic       uint16
 *   version     uint8
 *   type        uint8
 *   stream_id   uint8
 *   flags       uint8
 *   payload_len uint16
 */
static void build_packet(
    uint8_t *packet,
    uint8_t type,
    const uint8_t *payload,
    uint16_t payload_len
)
{
    packet[0] = (uint8_t)(VR_MAGIC >> 8);
    packet[1] = (uint8_t)(VR_MAGIC & 0xff);

    packet[2] = VR_VERSION;
    packet[3] = type;
    packet[4] = 0; /* stream_id */
    packet[5] = 0; /* flags */

    packet[6] = (uint8_t)(payload_len >> 8);
    packet[7] = (uint8_t)(payload_len & 0xff);

    if (payload_len > 0)
        memcpy(packet + HEADER_SIZE, payload, payload_len);
}

static int connect_and_establish(
    const char *host,
    uint16_t port,
    client_t *client
)
{
    int fd = -1;

    struct sockaddr_in addr;
    uint8_t connect_packet[HEADER_SIZE];
    uint8_t response[HEADER_SIZE];

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        fprintf(stderr,
                "[ERROR] Invalid IPv4 address: %s\n",
                host);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1)
        return -1;

    /*
     * Setup is intentionally blocking.
     * Connection establishment is NOT included in throughput timing.
     */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        close(fd);
        return -1;
    }

    build_packet(
        connect_packet,
        VR_PKT_CONNECT,
        NULL,
        0
    );

    if (send_all_blocking(fd, connect_packet, sizeof(connect_packet)) == -1)
    {
        close(fd);
        return -1;
    }

    if (recv_exact(fd, response, sizeof(response)) == -1)
    {
        close(fd);
        return -1;
    }

    uint16_t magic =
        ((uint16_t)response[0] << 8) |
        response[1];

    uint8_t version = response[2];
    uint8_t type = response[3];

    if (magic != VR_MAGIC ||
        version != VR_VERSION ||
        type != VR_PKT_CONNECT_ACK)
    {
        fprintf(
            stderr,
            "[ERROR] Invalid CONNECT_ACK "
            "(magic=0x%04x version=%u type=%u)\n",
            magic,
            version,
            type
        );

        close(fd);
        return -1;
    }

    if (set_nonblocking(fd) == -1)
    {
        close(fd);
        return -1;
    }

    client->fd = fd;
    client->batch_offset = 0;
    client->messages = 0;
    client->bytes = 0;
    client->failures = 0;

    return 0;
}

static void print_stats(
    double elapsed,
    uint64_t messages,
    uint64_t bytes,
    uint64_t failures,
    int active_clients
)
{
    double msg_rate =
        elapsed > 0.0
            ? (double)messages / elapsed
            : 0.0;

    double byte_rate =
        elapsed > 0.0
            ? (double)bytes / elapsed
            : 0.0;

    printf(
        "[%7.2fs] "
        "active=%5d "
        "rate=%10.0f msg/s "
        "network=%8.2f MiB/s "
        "total=%12llu "
        "failures=%llu\n",

        elapsed,
        active_clients,
        msg_rate,
        byte_rate / (1024.0 * 1024.0),

        (unsigned long long)messages,
        (unsigned long long)failures
    );

    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";

    uint16_t port = 22409;

    int client_count = DEFAULT_CLIENTS;
    int payload_size = DEFAULT_PAYLOAD_SIZE;
    int batch_packets = DEFAULT_BATCH_PACKETS;
    int duration = DEFAULT_DURATION;

    if (argc > 1)
        host = argv[1];

    if (argc > 2)
        port = (uint16_t)strtoul(argv[2], NULL, 10);

    if (argc > 3)
        client_count = atoi(argv[3]);

    if (argc > 4)
        payload_size = atoi(argv[4]);

    if (argc > 5)
        batch_packets = atoi(argv[5]);

    if (argc > 6)
        duration = atoi(argv[6]);

    if (client_count <= 0 ||
        payload_size < 0 ||
        payload_size > UINT16_MAX ||
        batch_packets <= 0 ||
        duration <= 0)
    {
        fprintf(stderr, "Invalid arguments.\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /*
     * Batch = N complete packets concatenated into one TCP write.
     */
    const size_t packet_size =
        HEADER_SIZE + (size_t)payload_size;

    const size_t batch_size =
        packet_size * (size_t)batch_packets;

    uint8_t *batch = malloc(batch_size);

    if (batch == NULL)
    {
        perror("malloc batch");
        return EXIT_FAILURE;
    }

    uint8_t *packet_payload = NULL;

    if (payload_size > 0)
    {
        packet_payload = malloc((size_t)payload_size);

        if (packet_payload == NULL)
        {
            perror("malloc payload");
            free(batch);
            return EXIT_FAILURE;
        }

        for (int i = 0; i < payload_size; i++)
            packet_payload[i] = (uint8_t)(i % 251);
    }

    uint8_t *packet = malloc(packet_size);

    if (packet == NULL)
    {
        perror("malloc packet");
        free(packet_payload);
        free(batch);
        return EXIT_FAILURE;
    }

    build_packet(
        packet,
        VR_PKT_PUBLISH,
        packet_payload,
        (uint16_t)payload_size
    );

    for (int i = 0; i < batch_packets; i++)
    {
        memcpy(
            batch + (size_t)i * packet_size,
            packet,
            packet_size
        );
    }

    free(packet);
    free(packet_payload);

    printf("\n");
    printf("========================================================================\n");
    printf(" VELORA C THROUGHPUT GENERATOR\n");
    printf("========================================================================\n\n");

    printf("[CONFIG] host              : %s\n", host);
    printf("[CONFIG] port              : %u\n", port);
    printf("[CONFIG] connections       : %d\n", client_count);
    printf("[CONFIG] payload           : %d bytes\n", payload_size);
    printf("[CONFIG] packet            : %zu bytes\n", packet_size);
    printf("[CONFIG] packets/send      : %d\n", batch_packets);
    printf("[CONFIG] batch             : %zu bytes\n", batch_size);
    printf("[CONFIG] duration          : %d seconds\n", duration);

    printf("\n[MODE]\n");
    printf("  One C process\n");
    printf("  One epoll instance\n");
    printf("  Non-blocking TCP sockets\n");
    printf("  Complete valid PUBLISH packets\n");
    printf("  Batched stream writes\n");
    printf("  No threads / no Python\n\n");

    /*
     * Allocate client state.
     */
    client_t *clients =
        calloc((size_t)client_count, sizeof(client_t));

    if (clients == NULL)
    {
        perror("calloc clients");
        free(batch);
        return EXIT_FAILURE;
    }

    printf("[SETUP] Creating %d connections...\n", client_count);

    int active_clients = 0;

    for (int i = 0; i < client_count; i++)
    {
        clients[i].fd = -1;

        if (connect_and_establish(host, port, &clients[i]) == -1)
        {
            fprintf(
                stderr,
                "[CONNECT FAIL] client=%d\n",
                i
            );

            continue;
        }

        active_clients++;

        if (active_clients % 1000 == 0 ||
            active_clients == client_count)
        {
            printf(
                "[CONNECT] %d/%d established\n",
                active_clients,
                client_count
            );
            fflush(stdout);
        }
    }

    printf("\n[SETUP COMPLETE]\n");
    printf(
        "  requested connections : %d\n",
        client_count
    );

    printf(
        "  established           : %d\n",
        active_clients
    );

    if (active_clients == 0)
    {
        fprintf(stderr, "[FATAL] No connections established.\n");

        free(clients);
        free(batch);

        return EXIT_FAILURE;
    }

    /*
     * One epoll instance for the client itself.
     *
     * EPOLLOUT is edge-triggered. When a socket becomes writable,
     * we keep sending until send() returns EAGAIN.
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
        free(batch);

        return EXIT_FAILURE;
    }

    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].fd < 0)
            continue;

        struct epoll_event ev = {0};

        ev.events = EPOLLOUT | EPOLLET;
        ev.data.u32 = (uint32_t)i;

        if (epoll_ctl(
                epfd,
                EPOLL_CTL_ADD,
                clients[i].fd,
                &ev
            ) == -1)
        {
            perror("epoll_ctl");

            close(clients[i].fd);
            clients[i].fd = -1;
            active_clients--;
        }
    }

    struct epoll_event *events =
        calloc(MAX_EVENTS, sizeof(struct epoll_event));

    if (events == NULL)
    {
        perror("calloc events");

        close(epfd);

        for (int i = 0; i < client_count; i++)
        {
            if (clients[i].fd >= 0)
                close(clients[i].fd);
        }

        free(clients);
        free(batch);

        return EXIT_FAILURE;
    }

    printf("\n[BENCHMARK] Starting...\n");
    printf("[BENCHMARK] Connection setup excluded from timing.\n\n");

    uint64_t total_messages = 0;
    uint64_t total_bytes = 0;
    uint64_t total_failures = 0;

    int benchmark_active = active_clients;

    double start = now_seconds();
    double next_report = start + 1.0;

    while (!stop_requested)
    {
        double current = now_seconds();

        if (current - start >= (double)duration)
            break;

        /*
         * Don't sleep here. epoll_wait() handles the backpressure case.
         */
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

        for (int e = 0; e < ready; e++)
        {
            uint32_t idx = events[e].data.u32;

            if (idx >= (uint32_t)client_count)
                continue;

            client_t *client = &clients[idx];

            if (client->fd < 0)
                continue;

            /*
             * A writable event means we should push as much data as
             * the kernel accepts until EAGAIN.
             */
            while (!stop_requested)
            {
                size_t remaining =
                    batch_size - client->batch_offset;

                ssize_t n = send(
                    client->fd,
                    batch + client->batch_offset,
                    remaining,
                    MSG_NOSIGNAL
                );

                if (n > 0)
                {
                    size_t previous_offset =
                        client->batch_offset;

                    client->batch_offset += (size_t)n;

                    client->bytes += (uint64_t)n;
                    total_bytes += (uint64_t)n;

                    /*
                     * Count only packets that have completely crossed
                     * the application -> kernel send boundary.
                     */
                    uint64_t completed_before =
                        previous_offset / packet_size;

                    uint64_t completed_after =
                        client->batch_offset / packet_size;

                    uint64_t completed =
                        completed_after - completed_before;

                    if (completed > 0)
                    {
                        client->messages += completed;
                        total_messages += completed;
                    }

                    /*
                     * Entire batch consumed.
                     */
                    if (client->batch_offset == batch_size)
                        client->batch_offset = 0;

                    continue;
                }

                if (n == -1 &&
                    (errno == EAGAIN ||
                     errno == EWOULDBLOCK))
                {
                    /*
                     * EPOLLET: return to epoll and wait until the
                     * socket transitions back to writable.
                     */
                    break;
                }

                if (n == -1 && errno == EINTR)
                    continue;

                /*
                 * Connection failure.
                 */
                client->failures++;
                total_failures++;

                fprintf(
                    stderr,
                    "[SEND FAIL] client=%u fd=%d errno=%d (%s)\n",
                    idx,
                    client->fd,
                    errno,
                    strerror(errno)
                );

                epoll_ctl(
                    epfd,
                    EPOLL_CTL_DEL,
                    client->fd,
                    NULL
                );

                close(client->fd);
                client->fd = -1;

                benchmark_active--;

                break;
            }
        }

        current = now_seconds();

        if (current >= next_report)
        {
            print_stats(
                current - start,
                total_messages,
                total_bytes,
                total_failures,
                benchmark_active
            );

            next_report += 1.0;
        }

        if (benchmark_active == 0)
        {
            fprintf(
                stderr,
                "[FATAL] All client connections failed.\n"
            );

            break;
        }
    }

    double end = now_seconds();
    double elapsed = end - start;

    /*
     * Final totals.
     */
    double msg_rate =
        elapsed > 0.0
            ? (double)total_messages / elapsed
            : 0.0;

    double byte_rate =
        elapsed > 0.0
            ? (double)total_bytes / elapsed
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
        "  Active connections      : %d\n",
        benchmark_active
    );

    printf(
        "  Failed sends            : %llu\n",
        (unsigned long long)total_failures
    );

    printf("\n");

    printf(
        "  Packet size             : %zu bytes\n",
        packet_size
    );

    printf(
        "  Payload size            : %d bytes\n",
        payload_size
    );

    printf(
        "  Batch size              : %d packets (%zu bytes)\n",
        batch_packets,
        batch_size
    );

    printf("\n");

    printf(
        "  Complete packets sent   : %llu\n",
        (unsigned long long)total_messages
    );

    printf(
        "  Bytes accepted by send  : %llu\n",
        (unsigned long long)total_bytes
    );

    printf(
        "  Average throughput      : %.0f messages/sec\n",
        msg_rate
    );

    printf(
        "  Network throughput      : %.2f MiB/sec\n",
        byte_rate / (1024.0 * 1024.0)
    );

    printf("\n");

    printf(
        "  Average packets/conn    : %.0f\n",
        active_clients > 0
            ? (double)total_messages / active_clients
            : 0.0
    );

    printf("\n");

    if (total_failures == 0)
        printf("[RESULT] PASS - no send failures\n");
    else
        printf(
            "[RESULT] WARNING - %llu send failures\n",
            (unsigned long long)total_failures
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

    free(events);
    close(epfd);
    free(clients);
    free(batch);

    return EXIT_SUCCESS;
}