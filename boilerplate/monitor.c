/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Tracks RSS memory usage of registered PIDs, emitting warnings at
 * soft limits and sending SIGKILL at hard limits.
 *
 * Lock choice: mutex
 *   The monitored list is accessed from two contexts:
 *     1. ioctl (process context, may sleep) - MONITOR_REGISTER / UNREGISTER
 *     2. timer callback (softirq / process context via workqueue on modern kernels)
 *
 *   mod_timer callbacks run in a tasklet-like context on older kernels but
 *   timer callbacks on Linux >= 4.15 run via the timer softirq, which does
 *   NOT sleep. A spinlock would therefore be sufficient; however, kzalloc
 *   inside the timer is avoided entirely (allocation is done in ioctl before
 *   lock acquisition), so the critical sections never sleep. We choose a
 *   mutex anyway because:
 *     - ioctl paths are process context and can sleep,
 *     - mutex gives better priority-inversion handling (PI futex on user side),
 *     - timer critical section is short (list walk + possible list_del), and
 *     - mutex_trylock() is used in the timer callback to avoid any theoretical
 *       deadlock if the callback were ever promoted to a sleepable context.
 *
 *   If a hard real-time requirement existed, switching to spinlock_bh would
 *   be the correct alternative.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

/* Fallback: older monitor_ioctl.h files may not define CONTAINER_ID_MAX */
#ifndef CONTAINER_ID_MAX
#define CONTAINER_ID_MAX 64
#endif

/*
 * del_timer_sync() was renamed to timer_delete_sync() in Linux 6.15.
 * Provide a compat shim so the module builds on both old and new kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
#define timer_delete_sync(t) del_timer_sync(t)
#endif

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ---------------------------------------------------------------
 * Linked-list node struct.
 * --------------------------------------------------------------- */
struct monitored_entry {
    pid_t           pid;
    char            container_id[CONTAINER_ID_MAX];
    unsigned long   soft_limit_bytes;   /* 0 = disabled              */
    unsigned long   hard_limit_bytes;   /* 0 = disabled              */
    bool            soft_warned;        /* true after first warning  */
    struct list_head node;
};

/* ---------------------------------------------------------------
 * Global list + lock.
 *
 * We use a mutex (see file-top rationale). The timer callback uses
 * mutex_trylock so it never blocks in softirq/timer context.
 * --------------------------------------------------------------- */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_lock);

/* --- internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t            dev_num;
static struct cdev      c_dev;
static struct class    *cl;

/* ---------------------------------------------------------------
 * RSS Helper
 *
 * Looks up the task by its host-namespace PID using find_get_pid(),
 * which searches init_pid_ns rather than the calling task's namespace.
 * This is correct because engine.c registers the host-side PID
 * (the value returned by clone()), not the in-container PID 1.
 *
 * find_vpid() would resolve against the current task's pid namespace,
 * which is wrong when called from a timer callback that runs in
 * arbitrary context — it would fail to find the container process and
 * return -1, causing the entry to be spuriously removed.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;
    struct pid *pid_struct;

    /*
     * find_get_pid() looks up the PID in init_pid_ns (the host namespace),
     * which is what we want — the PID registered by the supervisor is always
     * the host-namespace PID returned by clone().
     */
    pid_struct = find_get_pid(pid);
    if (!pid_struct)
        return -1;

    rcu_read_lock();
    task = pid_task(pid_struct, PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        put_pid(pid_struct);
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();
    put_pid(pid_struct);

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Soft-limit helper
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Hard-limit helper
 *
 * BUG FIX: The original used find_vpid() which resolves against the
 * current task's PID namespace.  When the timer fires in arbitrary
 * context the "current" namespace is not the supervisor's namespace,
 * so find_vpid() returns NULL and send_sig() is never called —
 * explaining why HARD LIMIT was logged but the process was not killed.
 *
 * Fix: use find_get_pid() (init_pid_ns lookup) + kill_pid(), which
 * is both namespace-correct and the preferred kernel API for sending
 * signals to a process by PID number.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct pid *pid_struct;

    /*
     * kill_pid() takes a struct pid * and sends the signal through the
     * proper signal-delivery machinery (respects signal masks, etc.).
     * We use SIGKILL (unblockable) so the container is guaranteed to die.
     */
    pid_struct = find_get_pid(pid);
    if (pid_struct) {
        kill_pid(pid_struct, SIGKILL, 1);
        put_pid(pid_struct);
    }

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu — SIGKILL sent\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    long rss;

    /*
     * Use trylock: if the ioctl path holds the mutex we simply skip this
     * tick rather than blocking in softirq/timer context. The next tick
     * (1 s later) will pick up any changes.
     */
    if (!mutex_trylock(&monitored_lock))
        goto reschedule;

    list_for_each_entry_safe(entry, tmp, &monitored_list, node) {
        rss = get_rss_bytes(entry->pid);

        /* Process has exited — clean up the entry. */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] PID %d exited, removing from container=%s\n",
                   entry->pid, entry->container_id);
            list_del(&entry->node);
            kfree(entry);
            continue;
        }

        /* Hard limit: kill then remove so we don't spam SIGKILL every tick. */
        if (entry->hard_limit_bytes > 0 &&
            (unsigned long)rss > entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->node);
            kfree(entry);
            continue;
        }

        /* Soft limit: warn once per crossing. */
        if (entry->soft_limit_bytes > 0 &&
            (unsigned long)rss > entry->soft_limit_bytes &&
            !entry->soft_warned) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = true;
        }

        /*
         * If RSS dropped back below the soft limit, reset the flag so a
         * future crossing triggers another warning.
         */
        if (entry->soft_limit_bytes > 0 &&
            (unsigned long)rss <= entry->soft_limit_bytes) {
            entry->soft_warned = false;
        }
    }

    mutex_unlock(&monitored_lock);

reschedule:
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitored_entry *entry, *tmp;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* Ensure container_id is NUL-terminated regardless of user input. */
    req.container_id[CONTAINER_ID_MAX - 1] = '\0';

    /* ------------------------------------------------------------------ */
    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *new_entry;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* Basic sanity: soft limit must be below hard limit when both set. */
        if (req.soft_limit_bytes > 0 && req.hard_limit_bytes > 0 &&
            req.soft_limit_bytes >= req.hard_limit_bytes) {
            printk(KERN_ERR
                   "[container_monitor] soft_limit (%lu) must be < hard_limit (%lu)\n",
                   req.soft_limit_bytes, req.hard_limit_bytes);
            return -EINVAL;
        }

        /* Allocate outside the lock so we can use GFP_KERNEL (may sleep). */
        new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
        if (!new_entry)
            return -ENOMEM;

        new_entry->pid              = req.pid;
        new_entry->soft_limit_bytes = req.soft_limit_bytes;
        new_entry->hard_limit_bytes = req.hard_limit_bytes;
        new_entry->soft_warned      = false;
        strncpy(new_entry->container_id, req.container_id, CONTAINER_ID_MAX - 1);
        new_entry->container_id[CONTAINER_ID_MAX - 1] = '\0';
        INIT_LIST_HEAD(&new_entry->node);

        mutex_lock(&monitored_lock);
        list_add_tail(&new_entry->node, &monitored_list);
        mutex_unlock(&monitored_lock);

        return 0;
    }

    /* ------------------------------------------------------------------ */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /*
     * Match on both PID and container_id for precision; a runtime could
     * manage multiple containers, so PID alone is insufficient when PIDs
     * are recycled across namespaces.
     */
    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, node) {
        if (entry->pid == (pid_t)req.pid &&
            strncmp(entry->container_id, req.container_id,
                    CONTAINER_ID_MAX) == 0) {
            list_del(&entry->node);
            kfree(entry);
            mutex_unlock(&monitored_lock);
            return 0;   /* found and removed */
        }
    }
    mutex_unlock(&monitored_lock);

    return -ENOENT;     /* no matching entry */
}

/* --- file operations --- */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Module Exit --- */
static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    /*
     * timer_delete_sync() guarantees the timer callback has finished and
     * will not fire again, so no timer tick can race with our list walk.
     * We still take the lock for correctness against any in-flight ioctl.
     */
    timer_delete_sync(&monitor_timer);

    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, node) {
        list_del(&entry->node);
        kfree(entry);
    }
    mutex_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
