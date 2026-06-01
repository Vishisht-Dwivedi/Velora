#include "velora/common.h"
#include "velora/net.h"

int vr_tcp_init()
{
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(22409);
    if(inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1)
    {
        return -1;
    }
    return 0;
}