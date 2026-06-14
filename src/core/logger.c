#include "velora/logger.h"

#ifdef VR_DEBUG

static FILE *log_ptr = NULL;
static pthread_mutex_t log_mutex;

vr_result_t vr_log_init(void)
{
    if ((log_ptr = fopen("log.txt", "a+")) == NULL)
    {
        fprintf(stderr, "Error while opening log file\n");
        return VR_ERROR;
    }

    if (pthread_mutex_init(&log_mutex, NULL) != 0)
    {
        fclose(log_ptr);
        log_ptr = NULL;
        fprintf(stderr, "Error while initializing mutex\n");
        return VR_ERROR;
    }

    vr_log(VR_LOG_INFO, "INITIALIZED");
    return VR_SUCCESS;
}

void vr_log(vr_log_level_t type, const char *fmt, ...)
{
    if (log_ptr == NULL)
        return;

    pthread_mutex_lock(&log_mutex);

    va_list args;
    va_start(args, fmt);

    switch (type)
    {
        case VR_LOG_INFO:
            fprintf(log_ptr, "INFO: ");
            break;

        case VR_LOG_ERROR:
            fprintf(log_ptr, "ERROR: ");
            break;

        case VR_LOG_WARN:
            fprintf(log_ptr, "WARN: ");
            break;

        default:
            fprintf(log_ptr, "UNKNOWN: ");
            break;
    }

    vfprintf(log_ptr, fmt, args);
    fprintf(log_ptr, "\n");

    if (type == VR_LOG_ERROR)
        fflush(log_ptr);

    va_end(args);

    pthread_mutex_unlock(&log_mutex);
}

void vr_log_close(void)
{
    if (log_ptr == NULL)
        return;

    pthread_mutex_lock(&log_mutex);

    fflush(log_ptr);
    fprintf(log_ptr, "EXIT\n");
    fclose(log_ptr);

    log_ptr = NULL;

    pthread_mutex_unlock(&log_mutex);
    pthread_mutex_destroy(&log_mutex);
}

#else

vr_result_t vr_log_init(void)
{
    return VR_SUCCESS;
}

void vr_log(vr_log_level_t type, const char *fmt, ...)
{
    (void)type;
    (void)fmt;
}

void vr_log_close(void)
{
}

#endif