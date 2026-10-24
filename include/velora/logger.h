#ifndef VR_LOGGER_H
#define VR_LOGGER_H

#include "velora/common.h"
#include <pthread.h>

typedef enum
{
    VR_LOG_TRACE = 0,
    VR_LOG_INFO = 1,
    VR_LOG_WARN = 2,
    VR_LOG_ERROR = 3
} vr_log_level_t;

typedef enum
{
    VR_COMPONENT_VELORA = 0,
    VR_COMPONENT_MTCP,
    VR_COMPONENT_AF_XDP,
    VR_COMPONENT_TCP,
    VR_COMPONENT_PROTOCOL,
    VR_COMPONENT_BUILD,
    VR_COMPONENT_UNKNOWN
} vr_log_component_t;

vr_result_t vr_log_init(void);
void vr_log_set_level(vr_log_level_t level);
void vr_log_set_path(const char *path);
void vr_log(vr_log_level_t type, const char *fmt, ...);
void vr_log_component(vr_log_component_t component,
                     vr_log_level_t type,
                     const char *fmt,
                     ...);
void vr_log_line(const char *line,
                vr_log_component_t component,
                vr_log_level_t type);
void vr_log_close(void);

#endif