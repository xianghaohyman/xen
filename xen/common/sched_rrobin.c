/****************************************************************************
 * Round Robin Scheduler for Xen
 *
 * by Mark Williamson (C) 2004 Intel Research Cambridge
 */

#include <xen/sched.h>
#include <xen/sched-if.h>
#include <hypervisor-ifs/sched_ctl.h>
#include <xen/ac_timer.h>
#include <xen/softirq.h>
#include <xen/time.h>
#include <xen/slab.h>

#define TIME_SLOP      (s32)MICROSECS(50)     /* allow time to slip a bit */

static s_time_t rr_slice = MILLISECS(10);

/* Only runqueue pointers and domain pointer*/
struct rrobin_dom_info
{
    struct list_head run_list;
    struct domain    *domain;
};

#define RR_INFO(d)      ((struct rrobin_dom_info *)d->sched_priv)
#define RUNLIST(d)      (struct list_head *)&(RR_INFO(d)->run_list)
#define RUNQUEUE(cpu)   RUNLIST(schedule_data[cpu].idle)

// TODO remove following line
static void rr_dump_cpu_state(int cpu);

/* SLAB cache for struct rrobin_dom_info objects */
static xmem_cache_t *dom_info_cache;


/* Ensures proper initialisation of the dom_info */
static void cache_constructor(void *arg1, xmem_cache_t *arg2, unsigned long arg3)
{
    struct rrobin_dom_info *dom_inf = (struct rrobin_dom_info*)arg1;
    dom_inf->run_list.next = NULL;
    dom_inf->run_list.prev = NULL;
}
            

/* Initialises the runqueues and creates the domain info cache */
static int rr_init_scheduler()
{
    int i;

    for ( i = 0; i < NR_CPUS; i++ )
        INIT_LIST_HEAD(RUNQUEUE(i));
   
    dom_info_cache = xmem_cache_create("FBVT dom info", 
                                        sizeof(struct rrobin_dom_info), 
                                        0, 0, cache_constructor, NULL);

    if(dom_info_cache == NULL)
    {
        printk("Could not allocate SLAB cache.\n");
        return -1;
    }
    return 0;                                                                
}

/* Allocates memory for per domain private scheduling data*/
static int rr_alloc_task(struct domain *d)
{
    d->sched_priv = xmem_cache_alloc(dom_info_cache);
    if ( d->sched_priv == NULL )
        return -1;

   return 0;
}

/* Setup the rr_dom_info */
static void rr_add_task(struct domain *p)
{
    struct rrobin_dom_info *inf;
    RR_INFO(p)->domain = p;
    inf = RR_INFO(p);
}

/* Frees memory used by domain info */
static void rr_free_task(struct domain *p)
{
    ASSERT( p->sched_priv != NULL );
    xmem_cache_free( dom_info_cache, p->sched_priv );
}

/* Initialises idle task */
static int rr_init_idle_task(struct domain *p)
{
    unsigned long flags;
    if(rr_alloc_task(p) < 0) return -1;
    rr_add_task(p);

    spin_lock_irqsave(&schedule_data[p->processor].schedule_lock, flags);
    set_bit(DF_RUNNING, &p->flags);
    if ( !__task_on_runqueue(RUNLIST(p)) )
         __add_to_runqueue_head(RUNLIST(p), RUNQUEUE(p->processor));
    spin_unlock_irqrestore(&schedule_data[p->processor].schedule_lock, flags);
    return 0;
}


/* Main scheduling function */
static task_slice_t rr_do_schedule(s_time_t now)
{
    struct domain *prev = current;
    int cpu = current->processor;
    
    task_slice_t ret;

    if(!is_idle_task(prev))
    {
        __del_from_runqueue(RUNLIST(prev));
    
        if ( domain_runnable(prev) )
            __add_to_runqueue_tail(RUNLIST(prev), RUNQUEUE(cpu));
    }
    
    ret.task = list_entry(  RUNQUEUE(cpu).next->next, 
                            struct rrobin_dom_info, 
                            run_list)->domain;
    ret.time = rr_slice;
    return ret;
}

/* Set/retrive control parameter(s) */
static int rr_ctl(struct sched_ctl_cmd *cmd)
{
    if ( cmd->direction == SCHED_INFO_PUT )
    {
        rr_slice = cmd->u.rrobin.slice;
    }
    else /* cmd->direction == SCHED_INFO_GET */
    {
        cmd->u.rrobin.slice = rr_slice;
    }
    
    return 0;
}

static void rr_dump_settings()
{
    printk("rr_slice = %llu ", rr_slice);
}

static void rr_sleep(struct domain *d)
{
    if ( test_bit(DF_RUNNING, &d->flags) )
        cpu_raise_softirq(d->processor, SCHEDULE_SOFTIRQ);
    else if ( __task_on_runqueue(RUNLIST(d)) )
        __del_from_runqueue(RUNLIST(d));
}

void rr_wake(struct domain *d)
{
    struct domain       *curr;
    s_time_t             now, min_time;
    int                  cpu = d->processor;

    /* If on the runqueue already then someone has done the wakeup work. */
    if ( unlikely(__task_on_runqueue(RUNLIST(d))))
        return;

    __add_to_runqueue_head(RUNLIST(d), RUNQUEUE(cpu));
    now = NOW();

    curr = schedule_data[cpu].curr;

    /* Currently-running domain should run at least for ctx_allow. */
    min_time = curr->lastschd + curr->min_slice;
    
    if ( is_idle_task(curr) || (min_time <= now) )
        cpu_raise_softirq(cpu, SCHEDULE_SOFTIRQ);
    else if ( schedule_data[cpu].s_timer.expires > (min_time + TIME_SLOP) )
        mod_ac_timer(&schedule_data[cpu].s_timer, min_time);
}


static void rr_dump_domain(struct domain *d)
{
    printk("%u has=%c ", d->domain,
           test_bit(DF_RUNNING, &d->flags) ? 'T':'F');
    printk("c=0x%X%08X\n", (u32)(d->cpu_time>>32), (u32)d->cpu_time);
}

static void rr_dump_cpu_state(int i)
{
    unsigned long flags;
    struct list_head *list, *queue;
    int loop = 0;
    struct rrobin_dom_info *d_inf;

    spin_lock_irqsave(&schedule_data[i].schedule_lock, flags);

    queue = RUNQUEUE(i);
    printk("QUEUE rq %lx   n: %lx, p: %lx\n",  (unsigned long)queue,
        (unsigned long) queue->next, (unsigned long) queue->prev);

    printk("%3d: ",loop++);
    d_inf = list_entry(queue, struct rrobin_dom_info, run_list);
    rr_dump_domain(d_inf->domain);
 
    list_for_each ( list, queue )
    {
        printk("%3d: ",loop++);
        d_inf = list_entry(list, struct rrobin_dom_info, run_list);
        rr_dump_domain(d_inf->domain);
    }
    spin_unlock_irqrestore(&schedule_data[i].schedule_lock, flags);
}


struct scheduler sched_rrobin_def = {
    .name     = "Round-Robin Scheduler",
    .opt_name = "rrobin",
    .sched_id = SCHED_RROBIN,
    
    .init_idle_task = rr_init_idle_task,
    .alloc_task     = rr_alloc_task,
    .add_task       = rr_add_task,
    .free_task      = rr_free_task,
    .init_scheduler = rr_init_scheduler,
    .do_schedule    = rr_do_schedule,
    .control        = rr_ctl,
    .dump_settings  = rr_dump_settings,
    .dump_cpu_state = rr_dump_cpu_state,
    .sleep          = rr_sleep,
    .wake           = rr_wake,
};


