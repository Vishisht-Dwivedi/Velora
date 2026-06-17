#include "shared/utilities.h"
vr_result_t vr_util_array_extend(void **array, size_t new_capacity, size_t element_size)
{
    void *temp = realloc(*array, new_capacity * element_size);
    if(temp == NULL)
    {
        vr_perror("Error while extending array memory");
        return VR_ERROR;
    }
    *array = temp;
    return VR_SUCCESS;
}