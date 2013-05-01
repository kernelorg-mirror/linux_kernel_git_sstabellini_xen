/*
 * Xen stolen ticks accounting.
 */
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/math64.h>
#include <linux/gfp.h>

#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>

#include <xen/events.h>
#include <xen/features.h>
#include <xen/interface/xen.h>
#include <xen/interface/vcpu.h>
#include <xen/xen-ops.h>

#define NS_PER_TICK	(1000000000LL / HZ)

/* runstate info updated by Xen */
static DEFINE_PER_CPU(struct vcpu_runstate_info, xen_runstate);

/* snapshots of runstate info */
static DEFINE_PER_CPU(struct vcpu_runstate_info, xen_runstate_snapshot);

/* unused ns of stolen and blocked time */
static DEFINE_PER_CPU(u64, xen_residual_stolen);
static DEFINE_PER_CPU(u64, xen_residual_blocked);

/* return an consistent snapshot of 64-bit time/counter value */
static u64 get64(const u64 *p)
{
	u64 ret;

	if (BITS_PER_LONG < 64) {
		u32 *p32 = (u32 *)p;
		u32 h, l;

		/*
		 * Read high then low, and then make sure high is
		 * still the same; this will only loop if low wraps
		 * and carries into high.
		 * XXX some clean way to make this endian-proof?
		 */
		do {
			h = p32[1];
			barrier();
			l = p32[0];
			barrier();
		} while (p32[1] != h);

		ret = (((u64)h) << 32) | l;
	} else
		ret = *p;

	return ret;
}

/*
 * Runstate accounting
 */
static void get_runstate_snapshot(struct vcpu_runstate_info *res)
{
	u64 state_time;
	struct vcpu_runstate_info *state;

	BUG_ON(preemptible());

	state = &__get_cpu_var(xen_runstate);

	/*
	 * The runstate info is always updated by the hypervisor on
	 * the current CPU, so there's no need to use anything
	 * stronger than a compiler barrier when fetching it.
	 */
	do {
		state_time = get64(&state->state_entry_time);
		barrier();
		*res = *state;
		barrier();
	} while (get64(&state->state_entry_time) != state_time);
}

/* return true when a vcpu could run but has no real cpu to run on */
bool xen_vcpu_stolen(int vcpu)
{
	return per_cpu(xen_runstate, vcpu).state == RUNSTATE_runnable;
}

void xen_setup_runstate_info(int cpu)
{
	struct vcpu_register_runstate_memory_area area;

	area.addr.v = &per_cpu(xen_runstate, cpu);

	if (HYPERVISOR_vcpu_op(VCPUOP_register_runstate_memory_area,
			       cpu, &area))
		BUG();
}

void xen_stolen_accounting(void)
{
	struct vcpu_runstate_info state;
	struct vcpu_runstate_info *snap;
	s64 blocked, runnable, offline, stolen;
	cputime_t ticks;

	get_runstate_snapshot(&state);

	WARN_ON(state.state != RUNSTATE_running);

	snap = &__get_cpu_var(xen_runstate_snapshot);

	/* work out how much time the VCPU has not been runn*ing*  */
	blocked = state.time[RUNSTATE_blocked] - snap->time[RUNSTATE_blocked];
	runnable = state.time[RUNSTATE_runnable] - snap->time[RUNSTATE_runnable];
	offline = state.time[RUNSTATE_offline] - snap->time[RUNSTATE_offline];

	*snap = state;

	/* Add the appropriate number of ticks of stolen time,
	   including any left-overs from last time. */
	stolen = runnable + offline + __this_cpu_read(xen_residual_stolen);

	if (stolen < 0)
		stolen = 0;

	ticks = iter_div_u64_rem(stolen, NS_PER_TICK, &stolen);
	__this_cpu_write(xen_residual_stolen, stolen);
	account_steal_ticks(ticks);

	/* Add the appropriate number of ticks of blocked time,
	   including any left-overs from last time. */
	blocked += __this_cpu_read(xen_residual_blocked);

	if (blocked < 0)
		blocked = 0;

	ticks = iter_div_u64_rem(blocked, NS_PER_TICK, &blocked);
	__this_cpu_write(xen_residual_blocked, blocked);
	account_idle_ticks(ticks);
}
