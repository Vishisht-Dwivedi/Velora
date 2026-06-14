#ifndef VR_LOGGER_H
#define VR_LOGGER_H

#include "velora/common.h"
#include <pthread.h>

typedef enum
{
    VR_LOG_INFO,
    VR_LOG_WARN,
    VR_LOG_ERROR
} vr_log_level_t;

vr_result_t vr_log_init(void);
void vr_log(vr_log_level_t type, const char *fmt, ...);
void vr_log_close(void);

#endif