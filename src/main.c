#include "velora/common.h"
#include "velora/logger.h"
int main()
{
    vr_log_init();
    printf("Hello world\n");
    vr_log_close();
}