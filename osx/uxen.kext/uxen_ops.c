#include "uxen.h"
#include "events.h"

#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <sys/proc.h>

#include <xen/domctl.h>
#include <xen/event_channel.h>

#define UXEN_DEFINE_SYMBOLS_PROTO
#include <uxen/uxen_link.h>

#include <i386/proc_reg.h> /* rdtsc64() */
#include <kern/clock.h> /* clock_get_calendar_microtime() */

static uint64_t host_counter_start;

uint8_t *frametable = NULL;
unsigned int frametable_size;
uint8_t *frametable_populated = NULL;
struct vm_info *dom0_vmi = NULL;
static void *percpu_area = NULL;
static unsigned int percpu_area_size;

uint32_t uxen_zero_mfn = ~0;

thread_t idle_thread[MAX_CPUS];
static semaphore_t idle_thread_exit[MAX_CPUS];
struct event_object idle_thread_event[MAX_CPUS];
static int resume_requested = 0;
static uint32_t idle_thread_suspended = 0;

static struct user_notification_event dummy_event = { };

struct host_event_channel {
    struct notification_event request;
    struct user_notification_event completed;
    struct host_event_channel *next;
};

static void *
map_page(xen_pfn_t mfn)
{

    /*
     * Mapping single pages, use the 1:1 physical memory mapping.
     */
    return (void *)physmap_pfn_to_va(mfn);
}

static uint64_t
unmap_page_va(const void *va)
{

    /*
     * Unmapping a single page is a noop.
     */
    return physmap_va_to_pfn(va);
}

static void *
map_page_range(struct vm_vcpu_info_shared *vcis, uint64_t n, uxen_pfn_t *mfns)
{
    /* struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis; */
    /* struct vm_info *vmi = (struct vm_info *)((uintptr_t)vcis & PAGE_MASK); */
    void *va;

    assert(n <= UXEN_MAP_PAGE_RANGE_MAX);

    va = map_pfn_array_from_pool(mfns, n);
    if (va)
        return va;

    /* dprintk("%s: vm%u.%u failed request %"PRId64" pages\n", __FUNCTION__, */
    /*         vmi->vmi_shared.vmi_domid, vcis->vci_vcpuid, n); */
    vcis->vci_map_page_range_requested = n;
    return NULL;
}

static uint64_t
unmap_page_range(struct vm_vcpu_info_shared *vcis, const void *va, uint64_t n,
                 uxen_pfn_t *mfns)
{

    unmap_pfn_array_from_pool(va, mfns);
    return 0;
}


static void __cdecl wake_vm(struct vm_vcpu_info_shared *vcis);
static int uxen_vmi_destroy_vm(struct vm_info *vmi);
static void quiesce_execution(void);
static void resume_execution(void);
static void uxen_flush_rcu(void);

static intptr_t
vm_info_compare_key(void *ctx, const void *b, const void *key)
{
    const struct vm_info * const pnp = b;
    const domid_t * const fhp = key;

    return pnp->vmi_shared.vmi_domid - *fhp;
}

static intptr_t
vm_info_compare_nodes(void *ctx, const void *parent, const void *node)
{
    const struct vm_info * const np = node;

    return vm_info_compare_key(ctx, parent, &np->vmi_shared.vmi_domid);
}

static const rb_tree_ops_t vm_info_rbtree_ops = {
    .rbto_compare_nodes = vm_info_compare_nodes,
    .rbto_compare_key = vm_info_compare_key,
    .rbto_node_offset = offsetof(struct vm_info, vmi_rbnode),
    .rbto_context = NULL
};

static void
ipi_dispatch(unsigned int vector)
{

    uxen_do_dispatch_ipi(0xff - vector);
}

static void
kick_cpu(uint64_t cpu, uint64_t vector)
{

    if (uxen_info->ui_running == 0)
	return;

    uxen_cpu_ipi(cpu, 0xff - vector);
}

static void
uxen_vcpu_ipi_cb(void *arg)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)arg;

    if (vci->vci_shared.vci_runnable == 0)
	return;
}

static void __cdecl
vcpu_ipi(struct vm_vcpu_info_shared *vcis)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis;

    if (vci->vci_shared.vci_runnable == 0)
	return;

    if (cpu_number() == vci->vci_host_cpu)
        return;

    uxen_cpu_call(vci->vci_host_cpu, uxen_vcpu_ipi_cb, vci);
}

static void __cdecl
vcpu_ipi_cancel(struct vm_vcpu_info_shared *vcis)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis;

    if (vci->vci_shared.vci_runnable == 0)
	return;
}

void
update_ui_host_counter(void)
{
    uint64_t now, wall;
    clock_sec_t sec;
    clock_usec_t usec;

    xnu_clock_gettimeofday(&sec, &usec);

    wall = sec;
    wall *= 1000000;
    wall += usec;
    wall *= 1000;

    now = mach_absolute_time();
    uxen_info->ui_host_counter_unixtime = wall;
    uxen_info->ui_host_counter_tsc = rdtsc64();
    uxen_info->ui_host_counter = now - host_counter_start;
}

static void
uxen_exception_event_all(void)
{
    affinity_t aff;
    struct vm_info *vmi, *tvmi;

    aff = uxen_lock();
    RB_TREE_FOREACH_SAFE(vmi, &uxen_devext->de_vm_info_rbtree, tvmi) {
        if (vmi->vmi_ioemu_exception_event.id != -1)
            signal_notification_event(&vmi->vmi_ioemu_exception_event);
    }
    uxen_unlock(aff);
}

static void
uxen_idle_thread_fn(void *context)
{
    unsigned int cpu = (unsigned int)context;
    unsigned int host_cpu;
    uint32_t increase;

    uxen_cpu_pin(cpu);

    dprintk("cpu%u: idle thread ready\n", cpu);

    while (uxen_info->ui_running) {
        uint32_t x;
        preemption_t i;
        uint64_t timeout, now;
        int had_timeout = 0;
        int ret;

        if (!cpu) {
            now = mach_absolute_time();
            timeout = now + uxen_info->ui_host_idle_timeout;
        }

        do {
            ret = event_wait(
                &idle_thread_event[cpu], EVENT_INTERRUPTIBLE,
                (!cpu && uxen_info->ui_host_idle_timeout &&
                 !idle_thread_suspended) ? timeout : EVENT_NO_TIMEOUT);
            if (ret == -1)
                had_timeout = 1;
            else if (ret) {
                dprintk("%s: cpu%u event_wait error (%d)\n", __FUNCTION__,
                        cpu, ret);
                goto out;
            }
            event_clear(&idle_thread_event[cpu]);
            MemoryBarrier();
            if (!cpu && resume_requested) {
                idle_thread_suspended = 0;
                printk_with_timestamp("power state change: resuming\n");
                update_ui_host_counter();
                /* uxen_call without de_executing and suspend_block call */
                while (uxen_pages_increase_reserve(&i, IDLE_RESERVE, &increase))
                    /* nothing */ ;
                try_call(, , uxen_do_resume_xen);
                resume_execution();
                resume_requested = 0;
                fast_event_signal(&uxen_devext->de_suspend_event);
                uxen_pages_decrease_reserve(i, increase);
                now = mach_absolute_time();
                for (host_cpu = 1; host_cpu < max_host_cpu; host_cpu++)
                    signal_idle_thread(host_cpu);
            }
        } while (idle_thread_suspended && uxen_info->ui_running);

        if (!uxen_info->ui_running)
            break;

        if (!cpu && uxen_info->ui_host_idle_timeout) {
            timeout = now + uxen_info->ui_host_idle_timeout;
            now = mach_absolute_time();
            if (now >= timeout) {
                update_ui_host_counter();
                had_timeout = 1;
                uxen_info->ui_host_idle_timeout = 0;
            } else
                uxen_info->ui_host_idle_timeout = timeout - now;
        }

        /* like uxen_call, except do not call suspend_block, but loop
         * to wait for idle_thread_event */
        if (uxen_pages_increase_reserve(&i, IDLE_RESERVE, &increase))
            x = 0;
        else {
            while ((x = uxen_devext->de_executing) == 0 ||
                   !OSCompareAndSwap(x, x + 1, &uxen_devext->de_executing)) {
                if (x == 0)
                    break;
            }
            if (x == 0)
                uxen_pages_decrease_reserve(i, increase);
        }
        if (x == 0) {
            /* Reset a timeout if we were going to signal that a
             * timeout had occurred. */
            if (!cpu && had_timeout)
                uxen_info->ui_host_idle_timeout = 1;
            continue;
        }
        try_call(, , uxen_do_run_idle_thread, had_timeout);
        if (OSDecrementAtomic(&uxen_devext->de_executing) == 1)
            fast_event_signal(&uxen_devext->de_suspend_event);
        uxen_pages_decrease_reserve(i, increase);
        if (!cpu) {
            if (idle_free_list && idle_free_free_list())
                signal_idle_thread(cpu);
            if (uxen_info->ui_exception_event_all) {
                uxen_info->ui_exception_event_all = 0;
                uxen_exception_event_all();
            }
        }
    }

  out:
    dprintk("cpu%u: idle thread exiting\n", cpu);

    semaphore_signal(idle_thread_exit[cpu]);
}

static void __cdecl
op_signal_idle_thread(uint64_t mask)
{
    unsigned int host_cpu;

    if (uxen_info->ui_running == 0)
	return;

    for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++) {
        if (mask & affinity_mask(host_cpu))
            signal_idle_thread(host_cpu);
    }
}

void
disable_preemption(preemption_t *i)
{

    *i = preemption_enabled();
    if (*i)
        xnu_disable_preemption();
}

void
enable_preemption(preemption_t i)
{

    if (i)
        xnu_enable_preemption();
}

void
set_host_preemption(uint64_t disable)
{

    if (disable) {
        if (!preemption_enabled())
            debug_break();
        xnu_disable_preemption();
    } else {
        if (preemption_enabled())
            debug_break();
        xnu_enable_preemption();
    }
}

static uint64_t __cdecl
host_needs_preempt(void)
{
    ast_t *ast = xnu_ast_pending();

    return (*ast & 0xF);
}

static void
uxen_vcpu_timer_cb(void *param0, void *param1)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)param0;

    if (vci->vci_shared.vci_runnable == 0)
	return;

    vci->vci_shared.vci_has_timer_interrupt = 1;
    wake_vm(&vci->vci_shared);
    /* Check if vcpu started running on another cpu after the timer
     * fired.  If so, interrupt it there. */
    if (vci->vci_host_cpu != cpu_number())
        vcpu_ipi(&vci->vci_shared);
}

static void __cdecl
set_vcpu_timer(struct vm_vcpu_info_shared *vcis, uint64_t expire)
{
    uint64_t now = mach_absolute_time();
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis;

    if (vcis->vci_runnable == 0)
	return;

    assert(!preemption_enabled());

    expire += now;

    if (cpu_number() != vci->vci_host_cpu) {
        dprintk("%s: cpu %d != %ld\n", __FUNCTION__, cpu_number(),
                vci->vci_host_cpu);
        /* ipi now?  mp_cpus_call to set timer on other cpu? */
    }

    set_timer(&vci->vci_timer, expire);
}

static uint64_t
get_host_counter(void)
{

    return mach_absolute_time() - host_counter_start;
}

static uint64_t
get_unixtime(void)
{
    clock_sec_t sec;
    clock_nsec_t nsec;

    clock_get_calendar_nanotime(&sec, &nsec);

    return (sec * 1000000000ULL) + nsec;
}

static void __cdecl
wake_vm(struct vm_vcpu_info_shared *vcis)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis;

    if (OSCompareAndSwap(1, 0, &vci->vci_shared.vci_host_halted))
        event_signal(&vci->vci_runnable);
}

int
suspend_block(preemption_t i, uint32_t pages, uint32_t *reserve_increase)
{
    int ret;

    ASSERT(!preemption_enabled());
    while (1) {
        MemoryBarrier();
        if (uxen_devext->de_executing)
            break;
        uxen_pages_decrease_reserve(i, *reserve_increase);
        if (!i)
            return -EAGAIN;
        fast_event_wait(&uxen_devext->de_resume_event, EVENT_UNINTERRUPTIBLE,
                        EVENT_NO_TIMEOUT);
        ret = uxen_pages_increase_reserve(&i, pages, reserve_increase);
        if (ret)
            return -ENOMEM;
    }
    return 0;
}

static void __cdecl
notify_exception(struct vm_info_shared *vmis)
{
    struct vm_info *vmi = (struct vm_info *)vmis;

    if (vmi->vmi_ioemu_exception_event.id != -1)
        signal_notification_event(&vmi->vmi_ioemu_exception_event);
}

static void __cdecl
notify_vram(struct vm_info_shared *vmis)
{
    struct vm_info *vmi = (struct vm_info *)vmis;

    if (vmi->vmi_ioemu_vram_event.id != -1)
        signal_notification_event(&vmi->vmi_ioemu_vram_event);
}

static uint64_t __cdecl
signal_event(struct vm_vcpu_info_shared *vcis, void *_hec,
             void * volatile *_wait_event)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis;
    struct host_event_channel *hec = (struct host_event_channel *)_hec;
    struct user_notification_event * volatile *wait_event =
        (struct user_notification_event * volatile *)_wait_event;

    if (!hec || hec->request.id == -1 || !vci->vci_shared.vci_runnable)
        return 1;

    if (hec->completed.notify_address && *wait_event != &hec->completed) {
        if (*wait_event && *wait_event != &dummy_event) {
            fail_msg("%s: nested waiting signal event", __FUNCTION__);
            return 1;
        }
        *wait_event = &hec->completed;
    }
    signal_notification_event(&hec->request);
    return 0;
}

static uint64_t __cdecl
check_ioreq(struct vm_vcpu_info_shared *vcis)
{
    struct vm_vcpu_info *vci = (struct vm_vcpu_info *)vcis;
    struct user_notification_event *event =
        (struct user_notification_event * volatile)vcis->vci_wait_event;
    int ret;

    if (event == &dummy_event || !vci->vci_shared.vci_runnable)
	return 1;
    ret = fast_event_state(&event->fast_ev);
    if (ret) {
        fast_event_clear(&event->fast_ev);
        vcis->vci_wait_event = &dummy_event;
    }
    return ret;
}

static int
create_thread(void (*continuation)(void *), void *arg,
              thread_t *thread, semaphore_t *exit_sem, int importance)
{
    thread_precedence_policy_data_t precedinfo;
    kern_return_t rc;

    rc = semaphore_create(kernel_task, exit_sem, 0, 0);
    if (rc != KERN_SUCCESS) {
        fail_msg("semaphore_create: %d", rc);
        *exit_sem = NULL;
        return ENOMEM;
    }

    rc = kernel_thread_start((thread_continue_t)continuation, arg, thread);
    if (rc != KERN_SUCCESS) {
        fail_msg("kernel_thread_start: %d", rc);
        *thread = NULL;
        semaphore_destroy(kernel_task, *exit_sem);
        *exit_sem = NULL;
        return ENOMEM;
    }
    thread_deallocate(*thread);

    precedinfo.importance = importance;
    rc = thread_policy_set(*thread, THREAD_PRECEDENCE_POLICY,
                            (thread_policy_t)&precedinfo,
                            THREAD_PRECEDENCE_POLICY_COUNT);
    if (rc != KERN_SUCCESS) {
        fail_msg("thread_policy_set THREAD_PRECEDENCE_POLICY");
        return EINVAL;
    }

    if (importance > 99) {
        thread_extended_policy_data_t extinfo;
        thread_time_constraint_policy_data_t timeinfo;
        uint64_t abstime;

        extinfo.timeshare = FALSE;
        rc = thread_policy_set(*thread, THREAD_EXTENDED_POLICY,
                                (thread_policy_t)&extinfo,
                                THREAD_EXTENDED_POLICY_COUNT);
        if (rc != KERN_SUCCESS) {
            fail_msg("thread_policy_set THREAD_EXTENDED_POLICY");
            return EINVAL;
        }

        timeinfo.period = 0;
        nanoseconds_to_absolutetime(100 * NSEC_PER_USEC, &abstime);
        timeinfo.computation = abstime;
        nanoseconds_to_absolutetime(500 * NSEC_PER_USEC, &abstime);
        timeinfo.constraint = abstime;
        timeinfo.preemptible = FALSE;
        rc = thread_policy_set(*thread, THREAD_TIME_CONSTRAINT_POLICY,
                                (thread_policy_t)&timeinfo,
                                THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (rc != KERN_SUCCESS) {
            fail_msg("thread_policy_set THREAD_TIME_CONSTRAINT_POLICY");
            return EINVAL;
        }
    }

    return 0;
}


void
uxen_op_init_free_allocs(void)
{

    if (uxen_info) {
        uxen_pages_clear();
        if (uxen_info->ui_hvm_io_bitmap) {
            kernel_free(uxen_info->ui_hvm_io_bitmap, UI_HVM_IO_BITMAP_SIZE);
            uxen_info->ui_hvm_io_bitmap = NULL;
        }
        if (uxen_info->ui_domain_array) {
            kernel_free(uxen_info->ui_domain_array,
                        uxen_info->ui_domain_array_pages << PAGE_SHIFT);
            uxen_info->ui_domain_array = NULL;
        }
    }

    if (idle_free_lock) {
        lck_spin_free(idle_free_lock, uxen_lck_grp);
        idle_free_lock = NULL;
    }
    fast_event_destroy(&dummy_event.fast_ev);

    if (frametable_populated) {
        dprintk("uxen mem: free frametable_populated\n");
        depopulate_frametable(frametable_size >> PAGE_SHIFT);
        kernel_free(frametable_populated,
                    ((frametable_size >> PAGE_SHIFT) + 7) / 8);
        frametable_populated = NULL;
    }
    if (frametable) {
	dprintk("uxen mem: free frametable\n");
        kernel_free_va(frametable, frametable_size >> PAGE_SHIFT);
	frametable = NULL;
    }
    if (populate_frametable_lock) {
        lck_mtx_free(populate_frametable_lock, uxen_lck_grp);
        populate_frametable_lock = NULL;
    }
    if (percpu_area) {
	dprintk("uxen mem: free percpu_area\n");
	kernel_free(percpu_area, percpu_area_size);
	percpu_area = NULL;
    }
    if (dom0_vmi) {
        dprintk("uxen mem: free dom0_vmi\n");
        kernel_free(dom0_vmi,
                    (size_t)ALIGN_PAGE_UP(
                        sizeof(struct vm_info) +
                        nr_host_cpus * sizeof(struct vm_vcpu_info)));
        dom0_vmi = NULL;
    }
    map_pfn_array_pool_clear();
    if (uxen_zero_mfn != ~0) {
	dprintk("uxen mem: free zero page\n");
	kernel_free_mfn(uxen_zero_mfn);
	uxen_zero_mfn = ~0;
    }
}

int
uxen_op_init(struct fd_assoc *fda)
{
    uint32_t max_pfn;
    uint32_t sizeof_percpu;
    unsigned int host_cpu;
    unsigned int cpu;
    affinity_t aff;
    int ret = 0;
    struct vm_vcpu_info_shared *vcis[UXEN_MAXIMUM_PROCESSORS];

    aff = uxen_lock();
    while (!OSCompareAndSwap(0, 1, &uxen_devext->de_initialised)) {
        uxen_unlock(aff);
        ret = fast_event_wait(&uxen_devext->de_init_done,
                              EVENT_INTERRUPTIBLE, EVENT_NO_TIMEOUT);
        if (ret)
            return EINTR;
        if (uxen_devext->de_initialised)
            return 0;
        aff = uxen_lock();
    }
    uxen_devext->de_executing = 2;
    fast_event_clear(&uxen_devext->de_init_done);
    uxen_unlock(aff);

    if (!fda->admin_access) {
        fail_msg("access denied");
        ret = EPERM;
        goto out;
    }

    printk("===============================================================\n");
    printk_with_timestamp("starting uXen driver version: %d.%d %s\n",
                          UXEN_DRIVER_VERSION_MAJOR, UXEN_DRIVER_VERSION_MINOR,
                          UXEN_DRIVER_VERSION_TAG);
    printk("uXen changeset: %s\n", UXEN_DRIVER_VERSION_CHANGESET);
    printk("===============================================================\n");

    if (!xnu_symbols_present) {
        fail_msg("can't proceed without XNU symbols");
        ret = EINVAL;
        goto out;
    }

#if defined(__UXEN_EMBEDDED__)
    ret = uxen_load_symbols();
    if (ret) {
        fail_msg("uxen_load_symbols failed: %d", ret);
	goto out;
    }
#endif

    if (uxen_info->ui_sizeof_struct_page_info == 0) {
        fail_msg("invalid sizeof(struct page_info)");
        ret = EINVAL;
        goto out;
    }

    rb_tree_init(&uxen_devext->de_vm_info_rbtree, &vm_info_rbtree_ops);

    max_pfn = get_max_pfn();
    dprintk("Max PFN = %x\n", max_pfn);

    if (uxen_cpu_set_active_mask(&uxen_info->ui_cpu_active_mask)) {
        ret = EINVAL;
        goto out;
    }

    ret = uxen_ipi_init(ipi_dispatch);
    if (ret) {
        fail_msg("uxen_ipi_init failed");
        goto out;
    }

    uxen_info->ui_printf = uxen_printk;

    uxen_info->ui_map_page = map_page;
    /* not called through to host */
    /* uxen_info->ui_unmap_page_va = unmap_page_va; */
    uxen_info->ui_map_page_global = map_page;
    uxen_info->ui_unmap_page_global_va = unmap_page_va;
    uxen_info->ui_map_page_range = map_page_range;
    uxen_info->ui_unmap_page_range = unmap_page_range;
    uxen_info->ui_mapped_global_va_pfn = physmap_va_to_pfn;

    uxen_info->ui_max_page = max_pfn;
    /* space for max_pfn virtual frametable entries */
    vframes_start = max_pfn;
    vframes_end = vframes_start + max_pfn;
    uxen_info->ui_max_vframe = vframes_end;

    uxen_info->ui_host_needs_preempt = host_needs_preempt;

    uxen_info->ui_on_selected_cpus = uxen_cpu_on_selected;
    uxen_info->ui_kick_cpu = kick_cpu;
    uxen_info->ui_kick_vcpu= vcpu_ipi;
    uxen_info->ui_kick_vcpu_cancel = vcpu_ipi_cancel;
    uxen_info->ui_wake_vm = wake_vm;

    uxen_info->ui_signal_idle_thread = op_signal_idle_thread;
    uxen_info->ui_set_timer_vcpu = set_vcpu_timer;

    uxen_info->ui_notify_exception = notify_exception;
    uxen_info->ui_notify_vram = notify_vram;
    uxen_info->ui_signal_event = signal_event;
    uxen_info->ui_check_ioreq = check_ioreq;

    uxen_info->ui_pagemap_needs_check = 0;

    uxen_info->ui_map_mfn = map_mfn;

    uxen_info->ui_user_access_ok = uxen_mem_user_access_ok;
    uxen_info->ui_smap_enabled = xnu_pmap_smap_enabled() ? 1 : 0;

    uxen_info->ui_rdmsr_safe = uxen_cpu_rdmsr_safe;

    printk("uxen mem:     maxpage %x\n", uxen_info->ui_max_page);

    ret = kernel_malloc_mfns(1, &uxen_zero_mfn, 1);
    if (ret != 1) {
        uxen_zero_mfn = ~0;
        fail_msg("kernel_malloc_mfns(zero_mfn) failed");
        ret = ENOMEM;
        goto out;
    }
    dprintk("uxen mem:   zero page %x\n", uxen_zero_mfn);

    /* frametable is sized based on vframes */
    frametable_size = vframes_end * uxen_info->ui_sizeof_struct_page_info;
    frametable_size = ((frametable_size + PAGE_SIZE-1) & ~(PAGE_SIZE-1));
    frametable = kernel_alloc_va(frametable_size >> PAGE_SHIFT);
    if (frametable == NULL || ((uintptr_t)frametable & (PAGE_SIZE - 1))) {
        fail_msg("kernel_alloc_va(frametable) failed");
        ret = ENOMEM;
	goto out;
    }
    uxen_info->ui_frametable = frametable;
    dprintk("uxen mem:  frametable %p - %p (0x%x/%dMB)\n", frametable,
            frametable + frametable_size, frametable_size,
	    frametable_size >> 20);

    frametable_populated = kernel_malloc(
        ((frametable_size >> PAGE_SHIFT) + 7) / 8);
    if (!frametable_populated) {
        fail_msg("kernel_malloc(frametable_populated) failed");
        ret = -ENOMEM;
        goto out;
    }
    dprintk("uxen mem: f-populated %p - %p (%dKB)\n", frametable_populated,
            frametable_populated + ((frametable_size >> PAGE_SHIFT) + 7) / 8,
            (((frametable_size >> PAGE_SHIFT) + 7) / 8) >> 10);
    populate_frametable_lock = lck_mtx_alloc_init(uxen_lck_grp, LCK_ATTR_NULL);
    if (!populate_frametable_lock) {
        fail_msg("populate frametable lck alloc failed");
        ret = ENOMEM;
        goto out;
    }
    populate_frametable_physical_memory();

    populate_vframes_lock = lck_mtx_alloc_init(uxen_lck_grp, LCK_ATTR_NULL);
    if (!populate_vframes_lock) {
        fail_msg("populate vframes lck alloc failed");
        ret = ENOMEM;
        goto out;
    }

    sizeof_percpu = (uxen_addr_per_cpu_data_end - uxen_addr_per_cpu_start +
                     PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    /* skip cpu 0 since uxen stores it in .data */
    percpu_area_size = (nr_host_cpus - 1) * sizeof_percpu;
    if (percpu_area_size) {
        percpu_area = kernel_malloc(percpu_area_size);
        if (percpu_area == NULL || ((uintptr_t)percpu_area & (PAGE_SIZE - 1))) {
            fail_msg("kernel_malloc(percpu_area) failed");
            ret = ENOMEM;
            goto out;
        }
        cpu = 0;
        for (host_cpu = 1; host_cpu < max_host_cpu; host_cpu++) {
            if ((uxen_info->ui_cpu_active_mask & affinity_mask(host_cpu)) == 0)
                continue;
            uxen_info->ui_percpu_area[host_cpu] =
                &percpu_area[cpu * sizeof_percpu];
            cpu++;
        }
        dprintk("uxen mem: percpu_area %p - %p\n", percpu_area,
                &percpu_area[percpu_area_size]);
    }

    uxen_info->ui_hvm_io_bitmap = kernel_malloc(UI_HVM_IO_BITMAP_SIZE);
    if (!uxen_info->ui_hvm_io_bitmap) {
        fail_msg("kernel_malloc(hvm_io_bitmap) failed");
        ret = ENOMEM;
        goto out;
    }

    uxen_info->ui_domain_array =
        kernel_malloc(uxen_info->ui_domain_array_pages << PAGE_SHIFT);
    if (!uxen_info->ui_domain_array) {
        fail_msg("kernel_malloc(ui_domain_array) failed");
        ret = -ENOMEM;
        goto out;
    }

    dom0_vmi = kernel_malloc(
        (size_t)ALIGN_PAGE_UP(sizeof(struct vm_info) +
                              nr_host_cpus * sizeof(struct vm_vcpu_info)));
    if (!dom0_vmi) {
        fail_msg("kernel_malloc(dom0_vmi) failed");
        ret = ENOMEM;
        goto out;
    }
    cpu = 0;
    for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++) {
        if ((uxen_info->ui_cpu_active_mask & affinity_mask(host_cpu)) == 0)
            continue;
        vcis[host_cpu] = &dom0_vmi->vmi_vcpus[cpu].vci_shared;
        cpu++;
    }

    map_pfn_array_pool_fill(0);

    uxen_info->ui_map_page_range_max_nr = UXEN_MAP_PAGE_RANGE_MAX;

    idle_free_lock = lck_spin_alloc_init(uxen_lck_grp, LCK_ATTR_NULL);
    if (!idle_free_lock) {
        fail_msg("idle free lck alloc failed");
        ret = ENOMEM;
        goto out;
    }

    ret = fast_event_init(&dummy_event.fast_ev, 1);
    if (ret) {
        fail_msg("dummy event init failed");
        goto out;
    }

    host_counter_start = mach_absolute_time();
    update_ui_host_counter();
    uxen_info->ui_host_counter_frequency = 1000000000;
    uxen_info->ui_get_unixtime = get_unixtime;
    uxen_info->ui_get_host_counter = get_host_counter;
    uxen_info->ui_host_timer_frequency = UXEN_HOST_TIMER_FREQUENCY;

    uxen_info->ui_host_gsoff_cpu = CPU_DATA_CPUNUMBER();
    uxen_info->ui_host_gsoff_current = CPU_DATA_CURRENT();

    fast_event_clear(&uxen_devext->de_shutdown_done);
    uxen_info->ui_running = 1;

    for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++) {
        if ((uxen_info->ui_cpu_active_mask & affinity_mask(host_cpu)) == 0)
            continue;
        idle_thread[host_cpu] = NULL;
        ret = event_init(&idle_thread_event[host_cpu], 0);
        if (ret) {
            fail_msg("create cpu%u idle thread event failed: %d", host_cpu,
                     ret);
            goto out;
        }
        ret = create_thread(uxen_idle_thread_fn, (void *)(uintptr_t)host_cpu,
                            &idle_thread[host_cpu],
                            &idle_thread_exit[host_cpu], 0);
        if (ret) {
            fail_msg("create cpu%u idle thread failed: %d", host_cpu, ret);
            event_destroy(&idle_thread_event[host_cpu]);
            goto out;
        }

        dprintk("setup cpu%u idle thread done\n", host_cpu);
    }

    /* init cpu dpc */

    fast_event_signal(&uxen_devext->de_resume_event);

    aff = uxen_cpu_pin_first();
    uxen_call(ret = (int), -EINVAL, NO_RESERVE, uxen_do_start_xen, NULL, 0,
              &dom0_vmi->vmi_shared, vcis);
    ret = uxen_translate_xen_errno(ret);
    uxen_cpu_unpin(aff);

    uxen_driver_publish_v4v_service();

    /* run idle thread to make it pick up the current timeout */
    signal_idle_thread(0);

    if (OSDecrementAtomic(&uxen_devext->de_executing) == 1)
        fast_event_signal(&uxen_devext->de_suspend_event);

  out:
    if (ret) {
        uxen_info->ui_running = 0;
        uxen_op_init_free_allocs();
        uxen_devext->de_executing = 0;
        uxen_devext->de_initialised = 0;
    }
    fast_event_signal(&uxen_devext->de_init_done);
    return ret;
}

int
uxen_op_shutdown(void)
{
    struct vm_info *vmi, *tvmi;
    unsigned int host_cpu;
    affinity_t aff;

    if (uxen_info == NULL)
        goto out;

    printk("%s: destroying VMs (core is %srunning)\n", __FUNCTION__,
           uxen_info->ui_running ? "" : "not ");

    aff = uxen_lock();
    RB_TREE_FOREACH_SAFE(vmi, &uxen_devext->de_vm_info_rbtree, tvmi) {
        dprintk("uxen shutdown: destroy vm%u\n", vmi->vmi_shared.vmi_domid);
        uxen_vmi_destroy_vm(vmi);
    }

    /* cleanup any templates which weren't freed before all clones
     * were destroyed */
    RB_TREE_FOREACH_SAFE(vmi, &uxen_devext->de_vm_info_rbtree, tvmi) {
        dprintk("uxen shutdown: cleanup vm%u\n", vmi->vmi_shared.vmi_domid);
        uxen_vmi_cleanup_vm(vmi);
    }

    if (RB_TREE_MIN(&uxen_devext->de_vm_info_rbtree)) {
        uxen_unlock(aff);
        goto out;
    }
    uxen_unlock(aff);

    if (!OSCompareAndSwap(1, 0, &uxen_devext->de_initialised)) {
        fast_event_wait(&uxen_devext->de_shutdown_done,
                        EVENT_UNINTERRUPTIBLE, EVENT_NO_TIMEOUT);
        goto out;
    }

    printk("%s: shutdown core\n", __FUNCTION__);

    uxen_flush_rcu();

    aff = uxen_lock();
    uxen_call((void), , NO_RESERVE, uxen_do_shutdown_xen);
    uxen_unlock(aff);

    uxen_info->ui_running = 0;

    for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++)
        signal_idle_thread(host_cpu);
    for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++) {
        if (!idle_thread[host_cpu])
            continue;
        semaphore_wait(idle_thread_exit[host_cpu]);
        semaphore_destroy(kernel_task, idle_thread_exit[host_cpu]);
        event_destroy(&idle_thread_event[host_cpu]);
        idle_thread[host_cpu] = NULL;
    }

    /* KeFlushQueuedDpcs(); */

    fast_event_signal(&uxen_devext->de_shutdown_done);

    printk("%s: shutdown done\n", __FUNCTION__);

#ifdef DEBUG_STRAY_PAGES
    if (frametable_populated) {
        dprintk("checking frametable for stray pages\n");
        find_stray_pages_in_frametable(frametable_size >> PAGE_SHIFT);
    }
#endif  /* DEBUG_STRAY_PAGES */

  out:
    return 0;
}

void
uxen_complete_shutdown(void)
{
    affinity_t aff;

    while (uxen_devext->de_initialised) {
        uxen_op_shutdown();

        aff = uxen_lock();
        if (RB_TREE_MIN(&uxen_devext->de_vm_info_rbtree)) {
            fast_event_clear(&uxen_devext->de_vm_cleanup_event);
            if (RB_TREE_MIN(&uxen_devext->de_vm_info_rbtree)) {
                uxen_unlock(aff);
                fast_event_wait(&uxen_devext->de_vm_cleanup_event,
                                EVENT_UNINTERRUPTIBLE, EVENT_NO_TIMEOUT);
                aff = uxen_lock();
            }
        }
        uxen_unlock(aff);
    }
}

int
uxen_op_wait_vm_exit(void)
{
    affinity_t aff;
    int ret;

    aff = uxen_lock();
    while (RB_TREE_MIN(&uxen_devext->de_vm_info_rbtree)) {
        fast_event_clear(&uxen_devext->de_vm_cleanup_event);
        if (RB_TREE_MIN(&uxen_devext->de_vm_info_rbtree)) {
            uxen_unlock(aff);
            ret = fast_event_wait(&uxen_devext->de_vm_cleanup_event,
                                  EVENT_INTERRUPTIBLE, EVENT_NO_TIMEOUT);
            if (ret)
                return EINTR;
            aff = uxen_lock();
        }
    }
    uxen_unlock(aff);

    return 0;
}

int
uxen_op_version(struct uxen_version_desc *uvd)
{

    uvd->uvd_driver_version_major = UXEN_DRIVER_VERSION_MAJOR;
    uvd->uvd_driver_version_minor = UXEN_DRIVER_VERSION_MINOR;
    memset(uvd->uvd_driver_version_tag, 0, sizeof(uvd->uvd_driver_version_tag));
    strlcpy(uvd->uvd_driver_version_tag, UXEN_DRIVER_VERSION_TAG,
            sizeof(uvd->uvd_driver_version_tag));

    return 0;
}

int
uxen_op_keyhandler(char *keys, unsigned int num)
{
    affinity_t aff;
    unsigned int i;
    int ret = 0;

    aff = uxen_exec_dom0_start();

    for (i = 0; i < num && keys[i]; i++) {
        unsigned char key = keys[i];
        switch (key) {
        case 'r':
            uxen_flush_rcu();
            ret = 0;
            break;
        default:
            uxen_call(ret = , -EINVAL, HYPERCALL_RESERVE,
                      uxen_do_handle_keypress, key);
            ret = uxen_translate_xen_errno(ret);
            break;
        }
        if (ret)
            break;
    }

    /* run idle thread in case a keyhandler changed a timer */
    signal_idle_thread(0);

    uxen_exec_dom0_end(aff);

    return ret;
}

int
uxen_op_create_vm(struct uxen_createvm_desc *ucd, struct fd_assoc *fda)
{
    struct vm_info *vmi;
    struct vm_vcpu_info *vci;
    struct vm_vcpu_info_shared *vcis[UXEN_MAXIMUM_VCPUS];
    affinity_t aff;
    unsigned int i;
    int ret = 0;

    if (fda->vmi) {
        fail_msg("domain %d: %" PRIuuid " vmi already exists",
                 fda->vmi->vmi_shared.vmi_domid, PRIuuid_arg(ucd->ucd_vmuuid));
        return EEXIST;
    }

    aff = uxen_exec_dom0_start();
    uxen_call(vmi = (struct vm_info *), -ENOENT, NO_RESERVE,
              uxen_do_lookup_vm, ucd->ucd_vmuuid);
    uxen_exec_dom0_end(aff);

    /* Found the vm or lookup failed */
    if (vmi && (intptr_t)vmi != -ENOENT) {
        if (is_neg_errno((intptr_t)vmi)) {
            fail_msg("%" PRIuuid " lookup failed: %d",
                     PRIuuid_arg(ucd->ucd_vmuuid), -(int)(intptr_t)vmi);
            return uxen_translate_xen_errno((intptr_t)vmi);
        }
        fail_msg("domain %d: %" PRIuuid " already exists",
                 vmi->vmi_shared.vmi_domid, PRIuuid_arg(ucd->ucd_vmuuid));
        return EEXIST;
    }

    vmi = kernel_malloc((size_t)ALIGN_PAGE_UP(
                            sizeof(struct vm_info) +
                            ucd->ucd_max_vcpus * sizeof(struct vm_vcpu_info)));
    if (!vmi) {
        ret = ENOMEM;
        goto out;
    }

    vmi->vmi_ioemu_exception_event.id = -1;
    vmi->vmi_ioemu_vram_event.id = -1;

    vmi->vmi_nrvcpus = ucd->ucd_max_vcpus;

    for (i = 0; i < vmi->vmi_nrvcpus; i++)
        vcis[i] = &vmi->vmi_vcpus[i].vci_shared;

    if (uxen_info->ui_xsave_cntxt_size) {
        vmi->vmi_shared.vmi_xsave = (uint64_t)kernel_malloc(
            (size_t)ALIGN_PAGE_UP(ucd->ucd_max_vcpus *
                                  uxen_info->ui_xsave_cntxt_size));
        if (!vmi->vmi_shared.vmi_xsave) {
            fail_msg("kernel_malloc(vmi_xsave, %d) failed",
                     ucd->ucd_max_vcpus * uxen_info->ui_xsave_cntxt_size);
            ret = ENOMEM;
            goto out;
        }
        vmi->vmi_shared.vmi_xsave_size =
            (size_t)ALIGN_PAGE_UP(ucd->ucd_max_vcpus *
                                  uxen_info->ui_xsave_cntxt_size);
    }

    vci = &vmi->vmi_vcpus[0];

    vci->vci_host_cpu = cpu_number();

    aff = uxen_cpu_pin_vcpu(vci, vci->vci_host_cpu);
    uxen_call(ret = (int), -EFAULT, SETUPVM_RESERVE, uxen_do_setup_vm,
              ucd, &vmi->vmi_shared, vcis);
    ret = uxen_translate_xen_errno(ret);
    uxen_cpu_unpin(aff);
    if (ret) {
	ret = EINVAL;
	goto out;
    }

    ret = fast_event_init(&vmi->vmi_notexecuting, 0);
    if (ret) {
        fail_msg("event_init vmi_notexecuting failed: %d", ret);
        goto out;
    }

    aff = uxen_lock();
    ret = (vmi == rb_tree_insert_node(&uxen_devext->de_vm_info_rbtree, vmi));
    uxen_unlock(aff);
    if (!ret) {
        fail_msg("domain ID already present in de_vm_info_rbtree");
        ret = EINVAL;
        goto out;
    }

    OSIncrementAtomic(&vmi->vmi_exists);
    OSIncrementAtomic(&vmi->vmi_alive);

    /* This reference will be dropped on vm destroy */
    OSIncrementAtomic(&vmi->vmi_active_references);

    for (i = 0; i < vmi->vmi_nrvcpus; i++) {
        vci = &vmi->vmi_vcpus[i];

        init_timer(&vci->vci_timer, uxen_vcpu_timer_cb, vci);
        vci->vci_timer_created = 1;
        vci->vci_shared.vci_wait_event = &dummy_event;

        ret = event_init(&vci->vci_runnable, 0);
        if (ret) {
            fail_msg("event_init vci_runnable failed: %d", ret);
            goto out;
        }
        vci->vci_shared.vci_runnable = 1;
    }

    ret = kernel_malloc_mfns(1, &vmi->vmi_undefined_mfn, 1);
    if (ret != 1) {
        fail_msg("kernel_malloc_mfns(vmi_undefined page) failed: %d", ret);
        vmi->vmi_undefined_mfn = ~0;
        ret = ENOMEM;
        goto out;
    }

    vmi->vmi_shared.vmi_runnable = 1;

    fda->vmi_owner = true;
    if (!(ucd->ucd_create_flags & XEN_DOMCTL_CDF_template))
        fda->vmi_destroy_on_close = TRUE;

    ret = 0;
  out:
    if (vmi && ret) {
        if (vmi->vmi_alive) {
            aff = uxen_lock();
            uxen_vmi_destroy_vm(vmi);
            uxen_unlock(aff);
        } else {
            aff = uxen_exec_dom0_start();
            uxen_call((void), , NO_RESERVE, uxen_do_destroy_vm,
                      ucd->ucd_vmuuid);
            uxen_exec_dom0_end(aff);
            if (vmi->vmi_shared.vmi_xsave) {
                kernel_free((void *)vmi->vmi_shared.vmi_xsave,
                            vmi->vmi_shared.vmi_xsave_size);
                vmi->vmi_shared.vmi_xsave = 0;
                vmi->vmi_shared.vmi_xsave_size = 0;
            }
            kernel_free(vmi, (size_t)ALIGN_PAGE_UP(
                            sizeof(struct vm_info) +
                            vmi->vmi_nrvcpus * sizeof(struct vm_vcpu_info)));
        }
        vmi = NULL;
    }

    if (vmi) {
        ucd->ucd_domid = vmi->vmi_shared.vmi_domid;

        /* This reference will be dropped on handle close */
        OSIncrementAtomic(&vmi->vmi_active_references);
        fda->vmi = vmi;
    }

    return ret;
}

int
uxen_op_target_vm(struct uxen_targetvm_desc *utd, struct fd_assoc *fda)
{
    struct vm_info *vmi;
    affinity_t aff;
    int ret = 0;

    if (fda->vmi)
        return EEXIST;

    aff = uxen_exec_dom0_start();
    uxen_call(vmi = (struct vm_info *), -ENOENT, NO_RESERVE,
              uxen_do_lookup_vm, utd->utd_vmuuid);
    uxen_exec_dom0_end(aff);

    /* Not found */
    if (!vmi)
        return ENOENT;
    /* or lookup failed */
    if (is_neg_errno((intptr_t)vmi))
        return uxen_translate_xen_errno((intptr_t)vmi);

    utd->utd_domid = vmi->vmi_shared.vmi_domid;

    /* This reference will be dropped on handle close */
    OSIncrementAtomic(&vmi->vmi_active_references);
    fda->vmi = vmi;

    return ret;
}

void
uxen_vmi_free(struct vm_info *vmi)
{
    uint32_t refs;

    do {
        refs = vmi->vmi_active_references;
    } while (cmpxchg(&vmi->vmi_active_references, refs, refs - 1) != refs);
    if (refs != 1)
        return;

    rb_tree_remove_node(&uxen_devext->de_vm_info_rbtree, vmi);

    /* KeFlushQueuedDpcs(); */

    while (vmi->vmi_host_event_channels != NULL) {
        struct host_event_channel *hec = vmi->vmi_host_event_channels;
        vmi->vmi_host_event_channels = hec->next;
        if (hec->completed.notify_address) {
            lck_mtx_lock(hec->completed.user_events->lck);
            rb_tree_remove_node(&hec->completed.user_events->events_rbtree,
                                &hec->completed);
            lck_mtx_unlock(hec->completed.user_events->lck);
        }
        kernel_free(hec, sizeof(*hec));
    }

    if (vmi->vmi_undefined_mfn != ~0) {
        kernel_free_mfn(vmi->vmi_undefined_mfn);
        vmi->vmi_undefined_mfn = ~0;
    }

    if (vmi->vmi_shared.vmi_xsave) {
        kernel_free((void *)vmi->vmi_shared.vmi_xsave,
                    vmi->vmi_shared.vmi_xsave_size);
        vmi->vmi_shared.vmi_xsave = 0;
        vmi->vmi_shared.vmi_xsave_size = 0;
    }

    logging_free(&vmi->vmi_logging_desc);

    dprintk("%s: vm%u vmi freed\n", __FUNCTION__, vmi->vmi_shared.vmi_domid);
    kernel_free(vmi, (size_t)ALIGN_PAGE_UP(
                    sizeof(struct vm_info) +
                    vmi->vmi_nrvcpus * sizeof(struct vm_vcpu_info)));

    fast_event_signal(&uxen_devext->de_vm_cleanup_event);
}

void
uxen_vmi_cleanup_vm(struct vm_info *vmi)
{
    int domid = vmi->vmi_shared.vmi_domid;
    unsigned int i;

    dprintk("%s: vm%u refs %d, running %d vcpus\n", __FUNCTION__, domid,
            vmi->vmi_active_references, vmi->vmi_running_vcpus);
    for (i = 0; i < vmi->vmi_nrvcpus; i++)
        dprintk("  vcpu vm%u.%u running %s\n", domid, i,
                vmi->vmi_vcpus[i].vci_shared.vci_runnable ? "yes" : "no");

    if (vmi->vmi_marked_for_destroy && uxen_vmi_destroy_vm(vmi)) {
        printk("%s: vm%u deferred by destroy\n", __FUNCTION__, domid);
        return;
    }

    printk("%s: vm%u cleanup complete\n", __FUNCTION__, domid);
}

static void
uxen_vmi_stop_running(struct vm_info *vmi)
{
    unsigned int i;

    dprintk("%s: vm%u has %d of %d vcpus running\n", __FUNCTION__,
            vmi->vmi_shared.vmi_domid, vmi->vmi_running_vcpus,
            vmi->vmi_nrvcpus);

    vmi->vmi_shared.vmi_runnable = 0;

    for (i = 0; i < vmi->vmi_nrvcpus; i++) {
        struct vm_vcpu_info *vci = &vmi->vmi_vcpus[i];
        struct user_notification_event *event =
            (struct user_notification_event * volatile)
            vci->vci_shared.vci_wait_event;

        dprintk("  vcpu vm%u.%u runnable %s\n", vmi->vmi_shared.vmi_domid, i,
                vci->vci_shared.vci_runnable ? "yes" : "no");

        if (!OSCompareAndSwap(1, 0, &vci->vci_shared.vci_runnable))
            continue;

        if (event != &dummy_event) {
            vci->vci_shared.vci_wait_event = &dummy_event;
            fast_event_signal(&event->fast_ev);
        }

        vci->vci_shared.vci_host_halted = 0;
        event_signal(&vci->vci_runnable);

        uxen_cpu_interrupt(1ULL << vci->vci_host_cpu);
    }

    /* KeFlushQueuedDpcs(); */

    fast_event_clear(&vmi->vmi_notexecuting);
    MemoryBarrier();
    if (vmi->vmi_running_vcpus)
        fast_event_wait(&vmi->vmi_notexecuting,
                        EVENT_UNINTERRUPTIBLE, EVENT_NO_TIMEOUT);

    printk("%s: vm%u all %d vcpus stopped (%d running)\n", __FUNCTION__,
           vmi->vmi_shared.vmi_domid, vmi->vmi_nrvcpus,
           vmi->vmi_running_vcpus);

    /* cancel timers only after all vcpus stopped */
    for (i = 0; i < vmi->vmi_nrvcpus; i++) {
        struct vm_vcpu_info *vci = &vmi->vmi_vcpus[i];

        if (OSCompareAndSwap(1, 0, &vci->vci_timer_created))
            cancel_timer(&vci->vci_timer);
    }
}

int
uxen_destroy_vm(struct vm_info *vmi)
{
    affinity_t aff;
    int ret;

    if (!OSCompareAndSwap(1, 0, &vmi->vmi_exists))
        return 0;

    aff = uxen_exec_dom0_start();
    uxen_call(ret = (int), -EINVAL, NO_RESERVE,
              uxen_do_destroy_vm, vmi->vmi_shared.vmi_uuid);
    uxen_exec_dom0_end(aff);
    ret = uxen_translate_xen_errno(ret);
    if (ret == ENOENT)
        ret = 0;
    if (ret) {
        printk("%s: vm%u not destroyed: %d\n", __FUNCTION__,
               vmi->vmi_shared.vmi_domid, ret);
        OSIncrementAtomic(&vmi->vmi_exists);
    }

    return ret;
}

static int
uxen_vmi_destroy_vm(struct vm_info *vmi)
{
    int ret;

    dprintk("%s: vm%u alive %s, refs %d, running %d vcpus\n", __FUNCTION__,
            vmi->vmi_shared.vmi_domid, vmi->vmi_alive ? "yes" : "no",
            vmi->vmi_active_references, vmi->vmi_running_vcpus);

    if (!OSCompareAndSwap(1, 0, &vmi->vmi_alive))
        return 0;

    vmi->vmi_marked_for_destroy = 1;

    uxen_vmi_stop_running(vmi);

    ret = uxen_destroy_vm(vmi);
    if (ret) {
        printk("%s: vm%u not destroyed: %d\n", __FUNCTION__,
               vmi->vmi_shared.vmi_domid, ret);
        OSIncrementAtomic(&vmi->vmi_alive);
        goto out;
    }

    printk("%s: vm%u destroyed\n", __FUNCTION__, vmi->vmi_shared.vmi_domid);
    vmi->vmi_marked_for_destroy = 0;

    uxen_vmi_free(vmi);

  out:
    return ret;
}

int
uxen_op_destroy_vm(struct uxen_destroyvm_desc *udd, struct fd_assoc *fda)
{
    struct vm_info *vmi;
    affinity_t aff, aff_locked;
    int ret = 0;

    /* allow destroy if admin or if this handle created the vm/vmi */
    if (!fda->admin_access &&
        (!fda->vmi || !fda->vmi_owner ||
         memcmp(udd->udd_vmuuid, fda->vmi->vmi_shared.vmi_uuid,
                sizeof(udd->udd_vmuuid)))) {
        fail_msg("access denied");
        ret = EPERM;
        goto out;
    }

    aff = uxen_lock();
    aff_locked = uxen_exec_dom0_start();
    uxen_call(vmi = (struct vm_info *), -ENOENT, NO_RESERVE,
              uxen_do_lookup_vm, udd->udd_vmuuid);
    uxen_exec_dom0_end(aff_locked);

    /* Found the vm or -errno means uuid not found or other error */
    if (is_neg_errno((intptr_t)vmi)) {
        ret = uxen_translate_xen_errno((intptr_t)vmi);
        uxen_unlock(aff);
        goto out;
    }

    if (vmi) {
        OSIncrementAtomic(&vmi->vmi_active_references);
        ret = uxen_vmi_destroy_vm(vmi);
        if (!ret)
            uxen_vmi_cleanup_vm(vmi);
        uxen_vmi_free(vmi);
        uxen_unlock(aff);
    } else {
        uxen_unlock(aff);
        aff = uxen_exec_dom0_start();
        uxen_call(ret = (int), -EINVAL, NO_RESERVE,
                  uxen_do_destroy_vm, udd->udd_vmuuid);
        uxen_exec_dom0_end(aff);
        ret = uxen_translate_xen_errno(ret);
    }

  out:
    return ret;
}

static int
thread_status_suspended(thread_t thread)
{
    static size_t thread_state_offset = 0;
    int status;

    if (thread_state_offset == 0) {
        thread_state_offset = THREAD_STATE();
    }

    status = *(int *)((char *)thread + thread_state_offset);

    return status & TH_SUSP;
}

static int
uxen_vcpu_thread_fn(struct vm_info *vmi, struct vm_vcpu_info *vci)
{
    affinity_t aff;
    int ret = 0;
    thread_t self = current_thread();
    ast_t *ast = xnu_ast_pending();

    while (!ret && vci->vci_shared.vci_runnable) {
        uint32_t x;
        uint32_t increase;
        preemption_t i;

        aff = uxen_cpu_pin_vcpu(vci, cpu_number());
        /* like uxen_call, except unpin cpu before re-enabling
         * preemption */
        if (uxen_pages_increase_reserve_extra(&i, VCPU_RUN_RESERVE,
                                              VCPU_RUN_EXTRA_RESERVE,
                                              &increase))
            x = 0;
        else
            while ((x = uxen_devext->de_executing) == 0 ||
                   !OSCompareAndSwap(x, x + 1, &uxen_devext->de_executing)) {
                if (suspend_block(i, VCPU_RUN_RESERVE +
                                  VCPU_RUN_EXTRA_RESERVE / 2, &increase)) {
                    x = 0;
                    break;
                }
            }
        if (x == 0) {
            uxen_cpu_unpin_vcpu(vci, aff);
            enable_preemption(i);
            continue;
        }
        try_call(ret = (int), -EFAULT, uxen_do_run_vcpu,
                 vmi->vmi_shared.vmi_domid, vci->vci_shared.vci_vcpuid);
        ret = uxen_translate_xen_errno(ret);
        if (ret)
            fail_msg("uxen_do_run_vcpu: vm%u.%u: ret %d",
                     vmi->vmi_shared.vmi_domid, vci->vci_shared.vci_vcpuid,
                     ret);
        if (OSDecrementAtomic(&uxen_devext->de_executing) == 1)
            fast_event_signal(&uxen_devext->de_suspend_event);
	uxen_cpu_unpin_vcpu(vci, aff);
        uxen_pages_decrease_reserve(i, increase);

        if (ret || !vci->vci_shared.vci_runnable)
	    break;

        if (vci->vci_shared.vci_map_page_range_requested) {
            /* dprintk("%s: vm%d.%d EMAPPAGERANGE cpu%d\n", __FUNCTION__, */
            /*         vmi->vmi_shared.vmi_domid, vci->vci_shared.vci_vcpuid, */
            /*         cpu_number()); */
            map_pfn_array_pool_fill(
                vci->vci_shared.vci_map_page_range_requested);
            vci->vci_shared.vci_map_page_range_requested = 0;
        }

        switch (vci->vci_shared.vci_run_mode) {
        case VCI_RUN_MODE_PROCESS_IOREQ: {
            struct user_notification_event *event =
                (struct user_notification_event * volatile)
                vci->vci_shared.vci_wait_event;
            if (event != &dummy_event) {
                ret = fast_event_wait(&event->fast_ev,
                                      EVENT_INTERRUPTIBLE, EVENT_NO_TIMEOUT);
                if (ret)
                    goto out;
                fast_event_clear(&event->fast_ev);
                vci->vci_shared.vci_wait_event = &dummy_event;
            }
            break;
        }
        case VCI_RUN_MODE_PREEMPT:
            if (thread_status_suspended(self)) {
                ret = EAGAIN;
                goto out;
            }
            /* Fallthrough */
        case VCI_RUN_MODE_YIELD:
            xnu_thread_block_reason(THREAD_CONTINUE_NULL, NULL, *ast);
            break;
        case VCI_RUN_MODE_SETUP:
        case VCI_RUN_MODE_HALT:
            event_clear(&vci->vci_runnable);
            MemoryBarrier();    /* ensure vci_host_halted was not pre-fetched */
            if (vci->vci_shared.vci_host_halted) {
                ret = event_wait(&vci->vci_runnable, EVENT_INTERRUPTIBLE,
                                 EVENT_NO_TIMEOUT);
                if (ret)
                    goto out;
            }
            break;
        case VCI_RUN_MODE_IDLE_WORK:
            /* nothing */
            break;
        case VCI_RUN_MODE_SHUTDOWN:
            ret = 0;
            goto out;
        case VCI_RUN_MODE_PAGEMAP_CHECK:
            /* nothing */
            break;
        case VCI_RUN_MODE_FREEPAGE_CHECK:
            /* nothing */
            break;
        case VCI_RUN_MODE_MAP_PAGE_REQUEST:
            /* nothing - handled above */
            break;
        case VCI_RUN_MODE_VFRAMES_CHECK:
            /* nothing */
            break;
        }
    }

  out:
    return ret;
}

int
uxen_op_execute(struct uxen_execute_desc *ued, struct vm_info *vmi)
{
    struct vm_vcpu_info *vci;
    int ret = ENOENT;

    if (ued->ued_vcpu >= UXEN_MAXIMUM_VCPUS) {
        fail_msg("invalid vm%u.%u", vmi->vmi_shared.vmi_domid, ued->ued_vcpu);
        return EINVAL;
    }

    OSIncrementAtomic(&vmi->vmi_running_vcpus);
    if (vmi->vmi_shared.vmi_runnable == 0)
        goto out;

    vci = &vmi->vmi_vcpus[ued->ued_vcpu];

    ret = uxen_vcpu_thread_fn(vmi, vci);

  out:
    printk("%s: exiting vm%u.%u (%d)\n", __FUNCTION__,
           vmi->vmi_shared.vmi_domid, ued->ued_vcpu, ret);

    if (OSDecrementAtomic(&vmi->vmi_running_vcpus) == 1)
        fast_event_signal(&vmi->vmi_notexecuting);
    return ret;
}

int
uxen_op_set_event(struct uxen_event_desc *ued, struct vm_info *vmi,
                  struct notification_event_queue *queue)
{
    struct notification_event *nev;
    int ret = ENOENT;

    OSIncrementAtomic(&vmi->vmi_running_vcpus);
    if (vmi->vmi_shared.vmi_runnable == 0)
        goto out;

    switch (ued->ued_id) {
    case UXEN_EVENT_EXCEPTION:
        nev = &vmi->vmi_ioemu_exception_event;
        break;
    case UXEN_EVENT_VRAM:
        nev = &vmi->vmi_ioemu_vram_event;
        break;
    default:
        fail_msg("unknown event %d", ued->ued_id);
        ret = EINVAL;
        goto out;
    }

    if (nev->id != -1) {
        fail_msg("cannot change event %d", ued->ued_id);
        ret = EINVAL;
        goto out;
    }

    ret = create_notification_event(queue, ued->ued_event, nev);
  out:
    if (OSDecrementAtomic(&vmi->vmi_running_vcpus) == 1)
        fast_event_signal(&vmi->vmi_notexecuting);
    return ret;
}

int
uxen_op_set_event_channel(
    struct uxen_event_channel_desc *uecd,
    struct vm_info *vmi, struct fd_assoc *fda,
    struct notification_event_queue *queue,
    struct user_notification_event_queue *user_events)
{
    struct host_event_channel *hec = NULL;
    struct evtchn_bind_host bind;
    int ret = ENOENT;

    OSIncrementAtomic(&vmi->vmi_running_vcpus);
    if (vmi->vmi_shared.vmi_runnable == 0)
        goto out;

    if (uecd->uecd_vcpu >= UXEN_MAXIMUM_VCPUS) {
        fail_msg("invalid vcpu");
        ret = EINVAL;
        goto out;
    }

    hec = kernel_malloc(sizeof(*hec));
    if (hec == NULL) {
        fail_msg("kernel_malloc failed");
        ret = ENOMEM;
        goto out;
    }

    memset(hec, 0, sizeof(*hec));
    ret = create_notification_event(queue, uecd->uecd_request_event,
                                    &hec->request);
    if (ret) {
        fail_msg("create_notification_event failed: %d", ret);
        goto out;
    }
    if (uecd->uecd_completed_event) {
        hec->completed.notify_address = uecd->uecd_completed_event;
        dprintk("%s: hec com addr %p\n", __FUNCTION__,
                hec->completed.notify_address);
        fast_event_init(&hec->completed.fast_ev, 0);
        hec->completed.user_events = user_events;
    }

    bind.remote_dom = vmi->vmi_shared.vmi_domid;
    bind.remote_port = uecd->uecd_port;
    bind.host_opaque = hec;
    ret = (int)uxen_dom0_hypercall(
        &vmi->vmi_shared, &fda->user_mappings,
        UXEN_UNRESTRICTED_ACCESS_HYPERCALL |
        (fda->admin_access ? UXEN_ADMIN_HYPERCALL : 0) |
        UXEN_SYSTEM_HYPERCALL |
        (fda->vmi_owner ? UXEN_VMI_OWNER : 0), __HYPERVISOR_event_channel_op,
        (uintptr_t)EVTCHNOP_bind_host, (uintptr_t)&bind);
    if (ret) {
        fail_msg("event_channel_op(bind_host) failed: %d", ret);
        destroy_notification_event(queue, &hec->request);
        goto out;
    }

    if (hec->completed.notify_address)
        fast_event_clear(&hec->completed.fast_ev);

    hec->next = vmi->vmi_host_event_channels;
    vmi->vmi_host_event_channels = hec;

    if (uecd->uecd_completed_event) {
        lck_mtx_lock(user_events->lck);
        rb_tree_insert_node(&user_events->events_rbtree, &hec->completed);
        lck_mtx_unlock(user_events->lck);
    }

    ret = 0;
  out:
    if (ret && hec)
        kernel_free(hec, sizeof(*hec));
    if (OSDecrementAtomic(&vmi->vmi_running_vcpus) == 1)
        fast_event_signal(&vmi->vmi_notexecuting);
    return ret;
}

int
uxen_op_poll_event(struct uxen_event_poll_desc *uepd,
                   struct notification_event_queue *events)
{
    uepd->signaled = poll_notification_event(events);
    return 0;
}

int
uxen_op_signal_event(void *addr,
                     struct user_notification_event_queue *user_events)
{
    struct user_notification_event *ev;
    int ret = 0;

    lck_mtx_lock(user_events->lck);
    ev = (struct user_notification_event *)
        rb_tree_find_node(&user_events->events_rbtree, addr);
    if (ev)
        fast_event_signal(&ev->fast_ev);
    else {
        fail_msg("unknown event with address %p", *(void **)addr);
        ret = ENOENT;
    }
    lck_mtx_unlock(user_events->lck);

    return ret;
}

int
uxen_op_query_vm(struct uxen_queryvm_desc *uqd)
{
    struct vm_info *vmi;
    affinity_t aff;

    aff = uxen_lock();

    vmi = rb_tree_find_node_geq(&uxen_devext->de_vm_info_rbtree,
                                &uqd->uqd_domid);
    if (vmi) {
        uqd->uqd_domid = vmi->vmi_shared.vmi_domid;
        memcpy(uqd->uqd_vmuuid, vmi->vmi_shared.vmi_uuid,
               sizeof(uqd->uqd_vmuuid));
    } else
        uqd->uqd_domid = -1;

    uxen_unlock(aff);

    return 0;
}

static void
quiesce_execution(void)
{
    fast_event_clear(&uxen_devext->de_resume_event);
    OSDecrementAtomic(&uxen_devext->de_executing);

    while (uxen_devext->de_executing || resume_requested) {
        fast_event_wait(&uxen_devext->de_suspend_event,
                        EVENT_UNINTERRUPTIBLE, EVENT_NO_TIMEOUT);
        fast_event_clear(&uxen_devext->de_resume_event);
        fast_event_clear(&uxen_devext->de_suspend_event);
    }
}

static void
resume_execution(void)
{

    if (OSIncrementAtomic(&uxen_devext->de_executing) >= 0)
        fast_event_signal(&uxen_devext->de_resume_event);
}

void
uxen_power_state(uint32_t suspend)
{

    if (!uxen_devext->de_initialised)
        return;

    if (!suspend) {
        resume_requested = 1;
        signal_idle_thread(0);
    } else {
        affinity_t aff;
        preemption_t i;
        int ret;

        ret = fast_event_wait(&uxen_devext->de_init_done,
                              EVENT_INTERRUPTIBLE, EVENT_NO_TIMEOUT);
        if (ret)
            return;
        if (!uxen_devext->de_initialised)
            return;

        printk_with_timestamp("power state change: suspending\n");

        idle_thread_suspended = suspend;

        fill_vframes();
        aff = uxen_lock();
        disable_preemption(&i);
        try_call(, , uxen_do_suspend_xen_prepare);
        enable_preemption(i);
        uxen_unlock(aff);

        quiesce_execution();

        /* now we're the only uxen thread executing, safe to take down vmx */
        fill_vframes();
        disable_preemption(&i);
        try_call(, , uxen_do_suspend_xen);
        enable_preemption(i);
    }
}

static void
uxen_flush_rcu(void)
{
    unsigned int host_cpu;
    int rcu_pending, cpu_rcu_pending;
    preemption_t i;
    affinity_t aff;

    aff = uxen_cpu_pin_current();

    for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++) {
        if ((uxen_info->ui_cpu_active_mask & affinity_mask(host_cpu)) == 0)
            continue;
        uxen_cpu_pin(host_cpu);
        fill_vframes();
        disable_preemption(&i);
        try_call(, , uxen_do_flush_rcu, 0);
        enable_preemption(i);
    }

    do {
        rcu_pending = 0;
        for (host_cpu = 0; host_cpu < max_host_cpu; host_cpu++) {
            if ((uxen_info->ui_cpu_active_mask & affinity_mask(host_cpu)) == 0)
                continue;
            uxen_cpu_pin(host_cpu);
            fill_vframes();
            disable_preemption(&i);
            try_call(cpu_rcu_pending = (int), 0, uxen_do_flush_rcu, 1);
            enable_preemption(i);
            rcu_pending |= cpu_rcu_pending;
        }
    } while (rcu_pending);

    uxen_cpu_unpin(aff);
}

int
uxen_op_map_host_pages(struct uxen_map_host_pages_desc *umhpd,
                       struct fd_assoc *fda)
{

    return map_host_pages(umhpd->umhpd_va, umhpd->umhpd_len,
                          umhpd->umhpd_gpfns, fda);
}

int
uxen_op_unmap_host_pages(struct uxen_map_host_pages_desc *umhpd,
                         struct fd_assoc *fda)
{

    return unmap_host_pages(umhpd->umhpd_va, umhpd->umhpd_len, fda);
}
