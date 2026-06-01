#ifndef VR_LOGGER_H
#define VR_LOGGER_H
#include "velora/common.h"
#include <pthread.h>

typedef enum
{
    VR_LOG_INFO,
    VR_LOG_ERROR,
    VR_LOG_WARN
} vr_log_level_t;

int vr_log_init(void);
void vr_log(vr_log_level_t type, const char *fmt, ...);
void vr_log_close(void);

#endif