
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

SEC("xdp")

int xdp_pass(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;
	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end) return XDP_PASS;
	// the first section of a packet is the ethernet headers
	bpf_printk("Ether Type: 0x%x", bpf_ntohs(eth->h_proto));
	bpf_printk("Ether src: %x", eth->h_source);
	bpf_printk("Ether dest: %x", eth->h_dest);
	// after initial 14 bytes we have the ip packet headers
	if (bpf_ntohs(eth->h_proto) != 0x0800) return XDP_PASS;
	struct iphdr *ip = (void *)(eth + 1);
	if ((void *)(ip + 1) > data_end) return XDP_PASS;
	bpf_printk("IPv4 packet-> IHL: %d TTL: %d protocol: %d", ip->ihl, ip->ttl, ip->protocol);
	bpf_printk("IPv4 packet-> total_len: %d src: %x dest: %x", bpf_ntohs(ip->tot_len), bpf_ntohl(ip->saddr), bpf_ntohl(ip->daddr));
	return XDP_PASS;
}
char LICENSE[] SEC("license") = "GPL";
