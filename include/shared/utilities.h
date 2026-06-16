#include <stdlib.h>
#include <stdbool.h>

typedef enum
{
    VR_SUCCESS,
    VR_ERROR,
    VR_INTERRUPTED,
    VR_EMPTY 
} vr_result_t;
//VR_EMPTY is used to indicate empty socket

vr_result_t vr_util_array_extend(void **array, size_t new_capacity, size_t element_size);