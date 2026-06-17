#ifndef VR_UTILS_H
#define VR_UTILS_H
#include <stdlib.h>
#include <stdbool.h>
#include "velora/logger.h"
#include "velora/error.h"
vr_result_t vr_util_array_extend(void **array, size_t new_capacity, size_t element_size);
#endif