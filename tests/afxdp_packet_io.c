#include "afxdp_packet_io.h"

#include <assert.h>
#include <stdint.h>

static void test_single_queue_policy(void)
{
    assert(afxdp_single_rx_queue_supported(1, 0, 64));
    assert(!afxdp_single_rx_queue_supported(4, 0, 64));
    assert(!afxdp_single_rx_queue_supported(1, 1, 64));
    assert(!afxdp_single_rx_queue_supported(1, 0, 0));
}

static void test_tx_reservation_deferral_and_retry(void)
{
    struct xsk_ring_prod tx = { 0 };
    struct xdp_desc descriptors[2] = {
        { .addr = UINT64_MAX, .len = UINT32_MAX, .options = UINT32_MAX },
        { .addr = UINT64_MAX, .len = UINT32_MAX, .options = UINT32_MAX }
    };
    uint32_t producer = 0;
    uint32_t consumer = 0;
    const uint64_t first_addrs[] = { 0x1000, 0x2000 };
    const uint64_t first_lens[] = { 64, 128 };
    const uint64_t pending_addr[] = { 0x3000 };
    const uint64_t pending_len[] = { 256 };

    tx.mask = 1;
    tx.size = 2;
    tx.producer = &producer;
    tx.consumer = &consumer;
    tx.ring = descriptors;
    tx.cached_cons = tx.size;

    assert(afxdp_tx_submit_pending(&tx, first_addrs, first_lens, 2) == 2);
    assert(producer == 2);
    assert(descriptors[0].addr == first_addrs[0]);
    assert(descriptors[1].addr == first_addrs[1]);

    assert(afxdp_tx_submit_pending(&tx, pending_addr, pending_len, 1) == 0);
    assert(producer == 2);
    assert(descriptors[0].addr == first_addrs[0]);
    assert(descriptors[1].addr == first_addrs[1]);
    assert(pending_addr[0] == 0x3000);
    assert(pending_len[0] == 256);

    consumer = 1;
    assert(afxdp_tx_submit_pending(&tx, pending_addr, pending_len, 1) == 1);
    assert(producer == 3);
    assert(descriptors[0].addr == pending_addr[0]);
    assert(descriptors[0].len == pending_len[0]);
}

int main(void)
{
    test_single_queue_policy();
    test_tx_reservation_deferral_and_retry();
    return 0;
}
