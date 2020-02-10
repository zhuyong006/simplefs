#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <net/tcp.h>

#define OPTSIZE	5
// saved_op保存跳转到原始函数的指令
char saved_op[OPTSIZE] = {0};

// jump_op保存跳转到hook函数的指令
char jump_op[OPTSIZE] = {0};

static unsigned int (*ptr_orig_conntrack_in)(const struct nf_hook_ops *ops, struct sk_buff *skb, const struct net_device *in, const struct net_device *out, const struct nf_hook_state *state);
static unsigned int (*ptr_ipv4_conntrack_in)(const struct nf_hook_ops *ops, struct sk_buff *skb, const struct net_device *in, const struct net_device *out, const struct nf_hook_state *state);

// stub函数，最终将会被保存指令的buffer覆盖掉
static unsigned int stub_ipv4_conntrack_in(const struct nf_hook_ops *ops, struct sk_buff *skb, const struct net_device *in, const struct net_device *out, const struct nf_hook_state *state)
{
	printk("hook stub conntrack\n");
	return 0;
}

// 这是我们的hook函数，当内核在调用ipv4_conntrack_in的时候，将会到达这个函数。
static unsigned int hook_ipv4_conntrack_in(const struct nf_hook_ops *ops, struct sk_buff *skb, const struct net_device *in, const struct net_device *out, const struct nf_hook_state *state)
{
	printk("hook conntrack\n");
	// 仅仅打印一行信息后，调用原始函数。
	return ptr_orig_conntrack_in(ops, skb, in, out, state);
}

static void *(*ptr_poke_smp)(void *addr, const void *opcode, size_t len);
static __init int hook_conn_init(void)
{
	s32 hook_offset, orig_offset;

	// 这个poke函数完成的就是重映射，写text段的事
	ptr_poke_smp = kallsyms_lookup_name("text_poke_smp");
	if (!ptr_poke_smp) {
		printk("%s LINE = %d\n",__func__,__LINE__);
		return -1;
	}

	// 嗯，我们就是要hook住ipv4_conntrack_in，所以要先找到它！
	ptr_ipv4_conntrack_in = kallsyms_lookup_name("ipv4_conntrack_in");
	if (!ptr_ipv4_conntrack_in) {
		printk("%s LINE = %d\n",__func__,__LINE__);
		return -1;
	}

	// 第一个字节当然是jump
	jump_op[0] = 0xe9;
	// 计算目标hook函数到当前位置的相对偏移
	hook_offset = (s32)((long)hook_ipv4_conntrack_in - (long)ptr_ipv4_conntrack_in - OPTSIZE);
	// 后面4个字节为一个相对偏移
	(*(s32*)(&jump_op[1])) = hook_offset;

	// 事实上，我们并没有保存原始ipv4_conntrack_in函数的头几条指令，
	// 而是直接jmp到了5条指令后的指令，对应上图，应该是指令buffer里没
	// 有old inst，直接就是jmp y了，为什么呢？后面细说。
	saved_op[0] = 0xe9;
	// 计算目标原始函数将要执行的位置到当前位置的偏移
	orig_offset = (s32)((long)ptr_ipv4_conntrack_in + OPTSIZE - ((long)stub_ipv4_conntrack_in + OPTSIZE));
	(*(s32*)(&saved_op[1])) = orig_offset;


	get_online_cpus();
	// 替换操作！
	ptr_poke_smp(stub_ipv4_conntrack_in, saved_op, OPTSIZE);
	ptr_orig_conntrack_in = stub_ipv4_conntrack_in;
	barrier();
	ptr_poke_smp(ptr_ipv4_conntrack_in, jump_op, OPTSIZE);
	put_online_cpus();

	return 0;
}
module_init(hook_conn_init);

static __exit void hook_conn_exit(void)
{
	get_online_cpus();
	ptr_poke_smp(ptr_ipv4_conntrack_in, saved_op, OPTSIZE);
	ptr_poke_smp(stub_ipv4_conntrack_in, jump_op, OPTSIZE);
	barrier();
	put_online_cpus();
}
module_exit(hook_conn_exit);

MODULE_DESCRIPTION("hook test");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1");
