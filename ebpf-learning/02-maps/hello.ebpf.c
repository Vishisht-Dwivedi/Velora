#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u32);
	__type(value, __u64);
} counter SEC(".maps");

SEC("tracepoint/syscalls/sys_enter_execve")

int count_execve(void *ctx) 
{
	__u32 pid = bpf_get_current_pid_tgid() >> 32;
	__u64 *count = bpf_map_lookup_elem(&counter, &pid);
	if (count)
	{
		(*count)++;
	} else {
		__u64 initial = 1;
		bpf_map_update_elem(&counter, &pid, &initial, BPF_ANY);
	}
	return 0;
}
char LICENSE[] SEC("license") = "GPL";
