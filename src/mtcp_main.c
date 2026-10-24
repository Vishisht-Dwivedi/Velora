#define _GNU_SOURCE

#include "velora/logger.h"
#include "velora/mtcp_runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    int read_fd;
    int saved_stdout_fd;
    int saved_stderr_fd;
    pthread_t reader_thread;
    int active;
} vr_mtcp_log_capture_t;

static volatile sig_atomic_t shutdown_requested;

static void *capture_mtcp_stderr_loop(void *arg)
{
    vr_mtcp_log_capture_t *capture = (vr_mtcp_log_capture_t *)arg;
    FILE *stream = fdopen(capture->read_fd, "r");
    char *line = NULL;
    size_t capacity = 0;

    if (stream == NULL) {
        vr_log_component(VR_COMPONENT_MTCP, VR_LOG_ERROR,
                         "failed to attach to mTCP stderr reader: %s",
                         strerror(errno));
        return NULL;
    }

    while (getline(&line, &capacity, stream) != -1) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length > 0) {
            vr_log_line(line, VR_COMPONENT_MTCP, VR_LOG_INFO);
        }
    }

    free(line);
    fclose(stream);
    return NULL;
}

static int start_mtcp_log_capture(vr_mtcp_log_capture_t *capture)
{
    int pipe_fds[2];

    if (capture == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(capture, 0, sizeof(*capture));
    if (pipe(pipe_fds) < 0) {
        return -1;
    }

    capture->read_fd = pipe_fds[0];
    capture->saved_stdout_fd = dup(STDOUT_FILENO);
    capture->saved_stderr_fd = dup(STDERR_FILENO);
    if (capture->saved_stdout_fd < 0 || capture->saved_stderr_fd < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    fflush(stdout);
    fflush(stderr);
    if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
        dup2(pipe_fds[1], STDERR_FILENO) < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    close(pipe_fds[1]);

    capture->active = 1;
    if (pthread_create(&capture->reader_thread, NULL,
                      capture_mtcp_stderr_loop, capture) != 0) {
        close(capture->read_fd);
        dup2(capture->saved_stdout_fd, STDOUT_FILENO);
        dup2(capture->saved_stderr_fd, STDERR_FILENO);
        close(capture->saved_stdout_fd);
        close(capture->saved_stderr_fd);
        capture->active = 0;
        return -1;
    }

    return 0;
}

static void stop_mtcp_log_capture(vr_mtcp_log_capture_t *capture)
{
    if (capture == NULL || !capture->active) {
        return;
    }

    fflush(stdout);
    fflush(stderr);
    if (capture->saved_stdout_fd >= 0) {
        dup2(capture->saved_stdout_fd, STDOUT_FILENO);
        close(capture->saved_stdout_fd);
        capture->saved_stdout_fd = -1;
    }
    if (capture->saved_stderr_fd >= 0) {
        dup2(capture->saved_stderr_fd, STDERR_FILENO);
        close(capture->saved_stderr_fd);
        capture->saved_stderr_fd = -1;
    }

    close(capture->read_fd);
    capture->read_fd = -1;
    pthread_join(capture->reader_thread, NULL);
    capture->active = 0;
}

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    shutdown_requested = 1;
}

static int install_shutdown_handlers(void)
{
    struct sigaction action = { 0 };

    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    return 0;
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    for (const char *digit = text; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return -1;
        }
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > UINT16_MAX) {
        return -1;
    }

    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    vr_mtcp_log_capture_t mtcp_log_capture = { .saved_stdout_fd = -1, .saved_stderr_fd = -1 };
    vr_mtcp_runtime_t runtime = VR_MTCP_RUNTIME_INITIALIZER;
    uint16_t port;
    int runtime_initialized = 0;
    int exit_status = EXIT_FAILURE;

    if (argc != 4 || parse_port(argc == 4 ? argv[3] : NULL, &port) < 0) {
        fprintf(stderr, "Usage: %s <mtcp-config-file> <bind-ipv4> <port>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (vr_log_init() != VR_SUCCESS) {
        return EXIT_FAILURE;
    }
    vr_log_component(VR_COMPONENT_BUILD,
                     VR_LOG_INFO,
                     "starting mTCP runtime config=%s bind=%s port=%u",
                     argv[1],
                     argv[2],
                     (unsigned int)port);
    if (start_mtcp_log_capture(&mtcp_log_capture) < 0) {
        vr_log_component(VR_COMPONENT_MTCP, VR_LOG_ERROR,
                         "failed to attach mTCP stderr capture: %s",
                         strerror(errno));
        goto cleanup;
    }
    if (install_shutdown_handlers() < 0) {
        perror("install signal handlers");
        goto cleanup;
    }
    if (vr_mtcp_runtime_init(&runtime, argv[1]) < 0) {
        perror("initialize mTCP runtime");
        goto cleanup;
    }
    runtime_initialized = 1;
    if (vr_mtcp_runtime_listen(&runtime, argv[2], port, 128) < 0) {
        perror("listen on mTCP runtime");
        goto cleanup;
    }

    while (!shutdown_requested) {
        for (;;) {
            int accepted_socket;
            int accept_result = vr_mtcp_runtime_accept_one(
                &runtime, &accepted_socket);
            if (accept_result < 0) {
                perror("accept mTCP connection");
                goto cleanup;
            }
            if (accept_result == 0) {
                break;
            }
            vr_log_component(VR_COMPONENT_TCP,
                             VR_LOG_INFO,
                             "accepted socket=%d",
                             accepted_socket);
        }

        size_t bytes_received;
        size_t packets_processed;
        size_t bytes_accepted_for_send;
        if (vr_mtcp_runtime_process_events(
                &runtime, 100, &bytes_received, &packets_processed,
                &bytes_accepted_for_send) < 0) {
            perror("process mTCP events");
            goto cleanup;
        }
    }

    exit_status = EXIT_SUCCESS;

cleanup:
    if (runtime_initialized && vr_mtcp_runtime_destroy(&runtime) < 0) {
        perror("destroy mTCP runtime");
        exit_status = EXIT_FAILURE;
    }
    stop_mtcp_log_capture(&mtcp_log_capture);
    vr_log_close();
    return exit_status;
}