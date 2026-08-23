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

#define VR_MAGIC        0x5789
#define VR_VERSION      1

#define VR_PKT_CONNECT      1
#define VR_PKT_CONNECT_ACK  2
#define VR_PKT_PING         3
#define VR_PKT_PONG         4
#define VR_PKT_PUBLISH      8

#define HEADER_SIZE         8

#define DEFAULT_CLIENTS     10000
#define DEFAULT_PACKETS     64
#define DEFAULT_PAYLOAD     64
#define DEFAULT_DURATION    30

#define MAX_EVENTS          1024
#define RX_BUFFER_SIZE      4096

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static double now_seconds(void)
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
 * Velora packet header:
 *
 * magic       : 2 bytes
 * version     : 1 byte
 * type        : 1 byte
 * stream_id   : 1 byte
 * flags       : 1 byte
 * payload_len : 2 bytes
 *
 * Total = 8 bytes.
 */
static void write_header(
    uint8_t *dst,
    uint8_t type,
    uint16_t payload_len
)
{
    dst[0] = (uint8_t)(VR_MAGIC >> 8);
    dst[1] = (uint8_t)(VR_MAGIC & 0xff);

    dst[2] = VR_VERSION;
    dst[3] = type;

    dst[4] = 0; /* stream_id */
    dst[5] = 0; /* flags */

    dst[6] = (uint8_t)(payload_len >> 8);
    dst[7] = (uint8_t)(payload_len & 0xff);
}

static int send_blocking(
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

static int recv_exact(
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

typedef struct
{
    int fd;
    int alive;

    /*
     * Number of packets in the current logical sequence that have
     * been sent but not yet confirmed by a PONG marker.
     */
    uint32_t awaiting_marker;

    /*
     * TCP receive buffer for PONG parsing.
     */
    uint8_t rx[RX_BUFFER_SIZE];
    size_t rx_len;

    uint64_t confirmed_packets;
    uint64_t errors;

} client_t;

static int establish_connection(
    const char *host,
    uint16_t port,
    client_t *client
)
{
    struct sockaddr_in addr;

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
     * Blocking setup. Excluded from benchmark timing.
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

    uint8_t connect_packet[HEADER_SIZE];
    uint8_t response[HEADER_SIZE];

    write_header(
        connect_packet,
        VR_PKT_CONNECT,
        0
    );

    if (send_blocking(
            fd,
            connect_packet,
            sizeof(connect_packet)
        ) == -1)
    {
        close(fd);
        return -1;
    }

    if (recv_exact(
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

    ev.events =
        EPOLLIN |
        EPOLLOUT |
        EPOLLET;

    ev.data.u32 = index;

    return epoll_ctl(
        epfd,
        EPOLL_CTL_ADD,
        client->fd,
        &ev
    );
}

/*
 * Build ONE logical workload sequence:
 *
 *   PUBLISH
 *   PUBLISH
 *   ...
 *   PUBLISH
 *   PING
 *
 * The PING is the completion marker.
 *
 * When a PONG comes back, every packet before it on that
 * TCP stream must have been parsed before the PING was handled.
 */
static size_t build_sequence(
    uint8_t *dst,
    int packet_count,
    const uint8_t *payload,
    uint16_t payload_len
)
{
    size_t offset = 0;

    for (int i = 0; i < packet_count; i++)
    {
        write_header(
            dst + offset,
            VR_PKT_PUBLISH,
            payload_len
        );

        offset += HEADER_SIZE;

        memcpy(
            dst + offset,
            payload,
            payload_len
        );

        offset += payload_len;
    }

    /*
     * Completion marker.
     */
    write_header(
        dst + offset,
        VR_PKT_PING,
        0
    );

    offset += HEADER_SIZE;

    return offset;
}

/*
 * Parse PONG responses from the TCP stream.
 */
static uint64_t drain_pongs(client_t *client)
{
    uint64_t completed_sequences = 0;

    while (client->rx_len >= HEADER_SIZE)
    {
        uint8_t *packet = client->rx;

        uint16_t magic =
            ((uint16_t)packet[0] << 8) |
            packet[1];

        uint8_t version = packet[2];
        uint8_t type = packet[3];
        uint8_t stream = packet[4];
        uint8_t flags = packet[5];

        uint16_t payload_len =
            ((uint16_t)packet[6] << 8) |
            packet[7];

        if (magic != VR_MAGIC ||
            version != VR_VERSION ||
            type != VR_PKT_PONG ||
            stream != 0 ||
            flags != 0 ||
            payload_len != 0)
        {
            client->errors++;
            return completed_sequences;
        }

        memmove(
            client->rx,
            client->rx + HEADER_SIZE,
            client->rx_len - HEADER_SIZE
        );

        client->rx_len -= HEADER_SIZE;

        if (client->awaiting_marker == 0)
        {
            client->errors++;
            continue;
        }

        /*
         * Every PONG confirms an entire sequence.
         */
        client->awaiting_marker = 0;
        completed_sequences++;
    }

    return completed_sequences;
}

static int drain_reads(
    client_t *client,
    uint64_t *completed_sequences
)
{
    while (client->alive)
    {
        if (client->rx_len == sizeof(client->rx))
        {
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

            *completed_sequences +=
                drain_pongs(client);

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
    int packets_per_sequence = DEFAULT_PACKETS;
    int payload_size = DEFAULT_PAYLOAD;
    int duration = DEFAULT_DURATION;

    if (argc > 1)
        host = argv[1];

    if (argc > 2)
        port = (uint16_t)strtoul(argv[2], NULL, 10);

    if (argc > 3)
        client_count = atoi(argv[3]);

    if (argc > 4)
        packets_per_sequence = atoi(argv[4]);

    if (argc > 5)
        payload_size = atoi(argv[5]);

    if (argc > 6)
        duration = atoi(argv[6]);

    if (client_count <= 0 ||
        packets_per_sequence <= 0 ||
        payload_size < 0 ||
        payload_size > UINT16_MAX ||
        duration <= 0)
    {
        fprintf(
            stderr,
            "Usage: %s "
            "[host] [port] [connections] "
            "[publish_packets] [payload_size] [duration]\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    const size_t publish_packet_size =
        HEADER_SIZE + (size_t)payload_size;

    /*
     * Sequence:
     *
     *   N × PUBLISH
     *   1 × PING
     */
    const size_t sequence_size =
        (size_t)packets_per_sequence *
        publish_packet_size +
        HEADER_SIZE;

    uint8_t *sequence =
        malloc(sequence_size);

    if (sequence == NULL)
    {
        perror("malloc sequence");
        return EXIT_FAILURE;
    }

    uint8_t *payload = NULL;

    if (payload_size > 0)
    {
        payload = malloc((size_t)payload_size);

        if (payload == NULL)
        {
            perror("malloc payload");
            free(sequence);
            return EXIT_FAILURE;
        }

        for (int i = 0; i < payload_size; i++)
            payload[i] = (uint8_t)(i % 251);
    }

    size_t actual_sequence_size =
        build_sequence(
            sequence,
            packets_per_sequence,
            payload,
            (uint16_t)payload_size
        );

    free(payload);

    printf("\n");
    printf("========================================================================\n");
    printf(" VELORA BLACK-BOX PARSER THROUGHPUT TEST\n");
    printf("========================================================================\n\n");

    printf("[CONFIG] host              : %s\n", host);
    printf("[CONFIG] port              : %u\n", port);
    printf("[CONFIG] connections       : %d\n", client_count);
    printf("[CONFIG] PUBLISH/sequence  : %d\n", packets_per_sequence);
    printf("[CONFIG] payload           : %d bytes\n", payload_size);
    printf("[CONFIG] packet            : %zu bytes\n", publish_packet_size);
    printf("[CONFIG] sequence          : %zu bytes\n", actual_sequence_size);
    printf("[CONFIG] duration          : %d seconds\n", duration);

    printf("\n[PROTOCOL MODEL]\n");
    printf("  CONNECT first -> ESTABLISHED\n");
    printf("  PUBLISH x N -> PING marker\n");
    printf("  PING -> PONG\n");
    printf("  Each PONG confirms one complete sequence was processed.\n");
    printf("  No malformed or protocol-invalid traffic is generated.\n\n");

    client_t *clients =
        calloc(
            (size_t)client_count,
            sizeof(client_t)
        );

    if (clients == NULL)
    {
        perror("calloc clients");
        free(sequence);
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

    if (established == 0)
    {
        fprintf(stderr, "[FATAL] No connections established.\n");

        free(clients);
        free(sequence);

        return EXIT_FAILURE;
    }

    printf("\n");
    printf("[SETUP COMPLETE]\n");
    printf(
        "  requested connections : %d\n",
        client_count
    );

    printf(
        "  established           : %d\n",
        established
    );

    /*
     * -------------------------------------------------------------
     * Client epoll
     * -------------------------------------------------------------
     */

    int epfd =
        epoll_create1(EPOLL_CLOEXEC);

    if (epfd == -1)
    {
        perror("epoll_create1");

        for (int i = 0; i < client_count; i++)
        {
            if (clients[i].fd >= 0)
                close(clients[i].fd);
        }

        free(clients);
        free(sequence);

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
            perror("epoll_ctl");

            clients[i].alive = 0;
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    }

    struct epoll_event events[MAX_EVENTS];

    printf("\n[BENCHMARK] Starting...\n");
    printf("[BENCHMARK] Connection setup excluded.\n");
    printf("[BENCHMARK] Counting CONFIRMED packet sequences only.\n\n");

    double start = now_seconds();
    double last_report = start;

    uint64_t completed_sequences = 0;
    uint64_t total_errors = 0;

    int active = established;

    while (!stop_requested)
    {
        double now = now_seconds();

        if (now - start >= (double)duration)
            break;

        int ready =
            epoll_wait(
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
            uint32_t index =
                events[e].data.u32;

            if (index >= (uint32_t)client_count)
                continue;

            client_t *client =
                &clients[index];

            if (!client->alive)
                continue;

            /*
             * First collect PONGs.
             */
            if (events[e].events & EPOLLIN)
            {
                uint64_t done = 0;

                if (drain_reads(
                        client,
                        &done
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
                    continue;
                }

                completed_sequences += done;
            }

            /*
             * Once a sequence is confirmed, immediately
             * send another sequence.
             */
            if (client->alive &&
                client->awaiting_marker == 0)
            {
                ssize_t sent =
                    send(
                        client->fd,
                        sequence,
                        actual_sequence_size,
                        MSG_NOSIGNAL
                    );

                if (sent == (ssize_t)actual_sequence_size)
                {
                    client->awaiting_marker =
                        (uint32_t)packets_per_sequence;

                    continue;
                }

                /*
                 * Whole sequence did not fit in one send().
                 *
                 * For this benchmark we intentionally require
                 * each logical sequence to be handed to the kernel
                 * as one contiguous write. This keeps packet-count
                 * accounting unambiguous.
                 *
                 * If the socket cannot accept the whole sequence
                 * yet, wait for EPOLLOUT.
                 */
                if (sent == -1 &&
                    (errno == EAGAIN ||
                     errno == EWOULDBLOCK))
                {
                    continue;
                }

                if (sent == -1 &&
                    errno == EINTR)
                {
                    continue;
                }

                client->errors++;

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

        now = now_seconds();

        /*
         * Periodic statistics.
         *
         * A completed sequence confirms:
         *
         *   packets_per_sequence PUBLISH packets
         *   +
         *   one PING
         *
         * were processed.
         */
        if (now - last_report >= 1.0)
        {
            double elapsed = now - start;

            uint64_t errors = total_errors;

            for (int i = 0; i < client_count; i++)
                errors += clients[i].errors;

            uint64_t packets_processed =
                completed_sequences *
                ((uint64_t)packets_per_sequence + 1);

            double packet_rate =
                elapsed > 0.0
                    ? (double)packets_processed / elapsed
                    : 0.0;

            printf(
                "[%7.2fs] "
                "active=%5d "
                "sequences=%10llu "
                "packets=%12llu "
                "packet_rate=%10.0f pkt/s "
                "errors=%llu\n",

                elapsed,
                active,

                (unsigned long long)
                    completed_sequences,

                (unsigned long long)
                    packets_processed,

                packet_rate,

                (unsigned long long)
                    errors
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
     * Final results
     * -------------------------------------------------------------
     */

    double end = now_seconds();
    double elapsed = end - start;

    uint64_t total_packets =
        completed_sequences *
        ((uint64_t)packets_per_sequence + 1);

    for (int i = 0; i < client_count; i++)
        total_errors += clients[i].errors;

    double packet_rate =
        elapsed > 0.0
            ? (double)total_packets / elapsed
            : 0.0;

    double publish_rate =
        elapsed > 0.0
            ? (double)
                (
                    completed_sequences *
                    (uint64_t)packets_per_sequence
                ) / elapsed
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
        "  PUBLISH packets/seq     : %d\n",
        packets_per_sequence
    );

    printf(
        "  Payload size            : %d bytes\n",
        payload_size
    );

    printf("\n");

    printf(
        "  Completed sequences     : %llu\n",
        (unsigned long long)
            completed_sequences
    );

    printf(
        "  Confirmed PUBLISH       : %llu\n",
        (unsigned long long)
            (
                completed_sequences *
                (uint64_t)packets_per_sequence
            )
    );

    printf(
        "  Confirming PINGs        : %llu\n",
        (unsigned long long)
            completed_sequences
    );

    printf(
        "  Confirmed packets total : %llu\n",
        (unsigned long long)
            total_packets
    );

    printf("\n");

    printf(
        "  PUBLISH throughput      : %.0f pkt/s\n",
        publish_rate
    );

    printf(
        "  Total packet throughput : %.0f pkt/s\n",
        packet_rate
    );

    printf(
        "  Total errors            : %llu\n",
        (unsigned long long)
            total_errors
    );

    printf(
        "  Avg packets/connection : %.0f\n",
        established > 0
            ? (double)total_packets / established
            : 0.0
    );

    printf("\n");

    if (total_errors == 0 &&
        active == established)
    {
        printf(
            "[RESULT] PASS - all connections remained healthy\n"
        );
    }
    else
    {
        printf(
            "[RESULT] WARNING - connection or protocol errors detected\n"
        );
    }

    printf("\n");
    printf("[INTERPRETATION]\n");
    printf(
        "  A sequence is counted only after its PONG marker arrives.\n"
    );
    printf(
        "  Each confirmed sequence represents %d PUBLISH + 1 PING\n",
        packets_per_sequence
    );
    printf(
        "  packets successfully traversing Velora's parser/protocol path.\n"
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
    free(sequence);

    return EXIT_SUCCESS;
}