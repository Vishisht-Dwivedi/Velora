#include "velora/logger.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#define VR_LOG_PATH_DEFAULT "log.txt"

static FILE *log_ptr = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static vr_log_level_t vr_log_threshold = VR_LOG_INFO;
static char log_path[512] = VR_LOG_PATH_DEFAULT;

static const char *vr_level_name(vr_log_level_t level)
{
    switch (level)
    {
        case VR_LOG_TRACE:
            return "TRACE";
        case VR_LOG_INFO:
            return "INFO";
        case VR_LOG_WARN:
            return "WARN";
        case VR_LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

static const char *vr_component_name(vr_log_component_t component)
{
    switch (component)
    {
        case VR_COMPONENT_VELORA:
            return "VELORA";
        case VR_COMPONENT_MTCP:
            return "MTCP";
        case VR_COMPONENT_AF_XDP:
            return "AF_XDP";
        case VR_COMPONENT_TCP:
            return "TCP";
        case VR_COMPONENT_PROTOCOL:
            return "PROTOCOL";
        case VR_COMPONENT_BUILD:
            return "BUILD";
        default:
            return "GENERIC";
    }
}

static void vr_emit_prefix(FILE *stream,
                          vr_log_component_t component,
                          vr_log_level_t level)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    fprintf(stream, "%ld.%06ld %-5s %-8s ",
            (long)now.tv_sec,
            (long)now.tv_usec,
            vr_level_name(level),
            vr_component_name(component));
}

void vr_log_set_level(vr_log_level_t level)
{
    vr_log_threshold = level;
}

void vr_log_set_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        snprintf(log_path, sizeof(log_path), "%s", VR_LOG_PATH_DEFAULT);
        return;
    }

    snprintf(log_path, sizeof(log_path), "%s", path);
}

vr_result_t vr_log_init(void)
{
    const char *path_override = getenv("VR_LOG_PATH");
    const char *threshold = getenv("VR_LOG_LEVEL");

    if (path_override != NULL && path_override[0] != '\0') {
        vr_log_set_path(path_override);
    }

    if (threshold != NULL && threshold[0] != '\0') {
        if (strcmp(threshold, "TRACE") == 0 || strcmp(threshold, "trace") == 0) {
            vr_log_threshold = VR_LOG_TRACE;
        } else if (strcmp(threshold, "INFO") == 0 || strcmp(threshold, "info") == 0) {
            vr_log_threshold = VR_LOG_INFO;
        } else if (strcmp(threshold, "WARN") == 0 || strcmp(threshold, "warn") == 0) {
            vr_log_threshold = VR_LOG_WARN;
        } else if (strcmp(threshold, "ERROR") == 0 || strcmp(threshold, "error") == 0) {
            vr_log_threshold = VR_LOG_ERROR;
        } else if (strcmp(threshold, "OFF") == 0 || strcmp(threshold, "off") == 0 ||
                   strcmp(threshold, "NONE") == 0 || strcmp(threshold, "none") == 0) {
            vr_log_threshold = (vr_log_level_t)4;
        }
    }

    if (vr_log_threshold >= (vr_log_level_t)4) {
        log_ptr = NULL;
        return VR_SUCCESS;
    }

    if ((log_ptr = fopen(log_path, "a+")) == NULL) {
        fprintf(stderr, "Error while opening log file %s: %s\n",
                log_path,
                strerror(errno));
        return VR_ERROR;
    }

    setvbuf(log_ptr, NULL, _IOLBF, 0);

    vr_log_component(VR_COMPONENT_BUILD,
                     VR_LOG_INFO,
                     "logger initialized path=%s level=%s pid=%ld",
                     log_path,
                     vr_level_name(vr_log_threshold),
                     (long)getpid());
    return VR_SUCCESS;
}

void vr_log_component(vr_log_component_t component,
                     vr_log_level_t type,
                     const char *fmt,
                     ...)
{
    if (log_ptr == NULL || fmt == NULL || type < vr_log_threshold) {
        return;
    }

    pthread_mutex_lock(&log_mutex);

    va_list args;
    va_start(args, fmt);
    vr_emit_prefix(log_ptr, component, type);
    vfprintf(log_ptr, fmt, args);
    fprintf(log_ptr, "\n");
    if (type == VR_LOG_ERROR)
        fflush(log_ptr);
    va_end(args);

    pthread_mutex_unlock(&log_mutex);
}

void vr_log_line(const char *line,
                 vr_log_component_t component,
                 vr_log_level_t type)
{
    if (line == NULL || log_ptr == NULL || type < vr_log_threshold) {
        return;
    }

    pthread_mutex_lock(&log_mutex);
    vr_emit_prefix(log_ptr, component, type);
    fprintf(log_ptr, "%s\n", line);
    fflush(log_ptr);
    pthread_mutex_unlock(&log_mutex);
}

void vr_log(vr_log_level_t type, const char *fmt, ...)
{
    if (log_ptr == NULL || fmt == NULL || type < vr_log_threshold) {
        return;
    }

    pthread_mutex_lock(&log_mutex);

    va_list args;
    va_start(args, fmt);
    vr_emit_prefix(log_ptr, VR_COMPONENT_VELORA, type);
    vfprintf(log_ptr, fmt, args);
    fprintf(log_ptr, "\n");
    if (type == VR_LOG_ERROR)
        fflush(log_ptr);
    va_end(args);

    pthread_mutex_unlock(&log_mutex);
}

void vr_log_close(void)
{
    if (log_ptr == NULL) {
        return;
    }

    pthread_mutex_lock(&log_mutex);
    vr_emit_prefix(log_ptr, VR_COMPONENT_BUILD, VR_LOG_INFO);
    fprintf(log_ptr, "logger closing\n");
    fflush(log_ptr);
    fclose(log_ptr);
    log_ptr = NULL;
    pthread_mutex_unlock(&log_mutex);
}

