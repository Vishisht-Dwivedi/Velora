#ifndef VR_COMMON_H
#define VR_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>

enum State
{
    RUNNING,
    SHUTDOWN_REQUESTED,
    EXITED
};

extern volatile sig_atomic_t running_status;

void handle_shutdown(int signal);


typedef enum
{
    VR_SUCCESS,
    VR_ERROR
} vr_result_t;

#endif