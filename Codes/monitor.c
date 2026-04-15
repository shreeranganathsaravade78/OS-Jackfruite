// monitor.c
//
// ══════════════════════════════════════════════════════════════
//  WHAT WE (STUDENTS) WROTE vs WHAT PROFESSOR GAVE
//
//  Professor gave: skeleton structure, monitor_ioctl.h,
//                  the idea of soft/hard limits
//  We wrote:       ALL the actual logic below
// ══════════════════════════════════════════════════════════════
//
// This is a Linux Kernel Module (LKM).
// It runs INSIDE the Linux kernel (not a normal program).
//
// What it does:
//   1. Creates a device file: /dev/container_monitor
//   2. engine.c registers container PIDs via ioctl
//   3. A kernel timer checks RSS (RAM usage) every 2 seconds
//   4. Soft limit exceeded → log warning to dmesg
//   5. Hard limit exceeded → send SIGKILL to kill the container
//
// Load it with:  sudo insmod monitor.ko
// Remove it with: sudo rmmod monitor

#include <linux/module.h>       // MODULE_LICENSE, module_init/exit
#include <linux/kernel.h>       // printk, pr_info
#include <linux/fs.h>           // file_operations, register_chrdev
#include <linux/uaccess.h>      // copy_from_user
#include <linux/slab.h>         // kmalloc, kfree
#include <linux/list.h>         // linked list (list_head)
#include <linux/mutex.h>        // mutex_lock/unlock
#include <linux/timer.h>        // timer_list, mod_timer
#include <linux/sched.h>        // find_task_by_vpid, send_sig
#include <linux/mm.h>           // get_task_mm, mm_struct
#include <linux/pid.h>          // find_get_pid
#include <linux/signal.h>       // SIGKILL, SIGTERM
#include <linux/jiffies.h>      // jiffies, HZ
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container memory monitor with soft/hard limits");

/* ── Device name and major number ── */
#define DEVICE_NAME  "container_monitor"
static int major_number;

/* ── One entry per registered container ── */
struct monitored_container {
    struct list_head list;      /* kernel linked list node */
    pid_t  pid;
    long   soft_limit_kb;
    long   hard_limit_kb;
    char   name[64];
    int    soft_warned;         /* 1 = we already sent soft-limit warning */
};

/* ── The linked list head and its protecting mutex ── */
static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_mutex);  /* only one thread touches the list at a time */

/* ── Periodic check timer ── */
static struct timer_list check_timer;
#define CHECK_INTERVAL_SEC  2   /* check every 2 seconds */

/* ────────────────────────────────────────────────────────────
 * get_rss_kb() - read how much RAM a process is using right now
 *
 * RSS = Resident Set Size = pages currently in physical RAM
 * We multiply page count by PAGE_SIZE and divide by 1024 → KB
 * ──────────────────────────────────────────────────────────── */
static long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_kb = 0;

    rcu_read_lock();
    task = pid_task(find_get_pid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;  /* process not found */
    }

    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return 0;

    /* get_mm_rss() returns number of resident pages */
    rss_kb = (get_mm_rss(mm) * PAGE_SIZE) / 1024;
    mmput(mm);
    return rss_kb;
}

/* ────────────────────────────────────────────────────────────
 * kill_container() - send SIGKILL to a container process
 * ──────────────────────────────────────────────────────────── */
static void kill_container(pid_t pid, const char *name)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_get_pid(pid), PIDTYPE_PID);
    if (task) {
        send_sig(SIGKILL, task, 0);
        pr_warn("container_monitor: [HARD LIMIT] killed container '%s' (pid=%d)\n",
                name, pid);
    }
    rcu_read_unlock();
}

/* ────────────────────────────────────────────────────────────
 * timer_check_callback() - called every CHECK_INTERVAL_SEC seconds
 *
 * This is the heart of the kernel monitor.
 * It walks through every registered container and:
 *   - checks if process still exists
 *   - reads RSS
 *   - warns if over soft limit (once)
 *   - kills  if over hard limit
 * ──────────────────────────────────────────────────────────── */
static void timer_check_callback(struct timer_list *t)
{
    struct monitored_container *entry, *tmp;
    long rss_kb;

    mutex_lock(&list_mutex);

    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        rss_kb = get_rss_kb(entry->pid);

        if (rss_kb < 0) {
            /* Process is gone — remove stale entry */
            pr_info("container_monitor: container '%s' (pid=%d) no longer exists, removing\n",
                    entry->name, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* ── Soft limit check ── */
        if (!entry->soft_warned && rss_kb > entry->soft_limit_kb) {
            pr_warn("container_monitor: [SOFT LIMIT] container '%s' (pid=%d) "
                    "using %ld KB, soft limit is %ld KB\n",
                    entry->name, entry->pid, rss_kb, entry->soft_limit_kb);
            entry->soft_warned = 1;  /* only warn once */
        }

        /* ── Hard limit check ── */
        if (rss_kb > entry->hard_limit_kb) {
            pr_warn("container_monitor: [HARD LIMIT] container '%s' (pid=%d) "
                    "using %ld KB, hard limit is %ld KB — killing\n",
                    entry->name, entry->pid, rss_kb, entry->hard_limit_kb);
            kill_container(entry->pid, entry->name);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        pr_info("container_monitor: '%s' (pid=%d) RSS=%ld KB "
                "(soft=%ld, hard=%ld)\n",
                entry->name, entry->pid, rss_kb,
                entry->soft_limit_kb, entry->hard_limit_kb);
    }

    mutex_unlock(&list_mutex);

    /* Re-arm the timer for next check */
    mod_timer(&check_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ────────────────────────────────────────────────────────────
 * ioctl handler — receives commands from engine.c
 * ──────────────────────────────────────────────────────────── */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct container_info info;
    struct monitored_container *entry, *tmp;
    pid_t  unregister_pid;

    switch (cmd) {

    case MONITOR_REGISTER:
        /* engine.c wants us to start watching a new container */
        if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
            return -EFAULT;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid          = info.pid;
        entry->soft_limit_kb = info.soft_limit_kb;
        entry->hard_limit_kb = info.hard_limit_kb;
        entry->soft_warned  = 0;
        strncpy(entry->name, info.name, 63);
        entry->name[63] = '\0';

        mutex_lock(&list_mutex);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&list_mutex);

        pr_info("container_monitor: registered '%s' pid=%d soft=%ldKB hard=%ldKB\n",
                entry->name, entry->pid,
                entry->soft_limit_kb, entry->hard_limit_kb);
        return 0;

    case MONITOR_UNREGISTER:
        /* engine.c wants us to stop watching a container */
        if (copy_from_user(&unregister_pid, (void __user *)arg, sizeof(pid_t)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == unregister_pid) {
                pr_info("container_monitor: unregistered '%s' (pid=%d)\n",
                        entry->name, entry->pid);
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&list_mutex);
        return 0;

    default:
        return -ENOTTY;
    }
}

/* ── File operations table for our device ── */
static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ────────────────────────────────────────────────────────────
 * module_init — called when you do: sudo insmod monitor.ko
 * ──────────────────────────────────────────────────────────── */
static int __init monitor_init(void)
{
    major_number = register_chrdev(0, DEVICE_NAME, &monitor_fops);
    if (major_number < 0) {
        pr_err("container_monitor: failed to register device\n");
        return major_number;
    }

    pr_info("container_monitor: loaded (major=%d)\n", major_number);
    pr_info("container_monitor: create device with:\n");
    pr_info("  sudo mknod /dev/container_monitor c %d 0\n", major_number);
    pr_info("  sudo chmod 666 /dev/container_monitor\n");

    /* Start the periodic memory check timer */
    timer_setup(&check_timer, timer_check_callback, 0);
    mod_timer(&check_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    return 0;
}

/* ────────────────────────────────────────────────────────────
 * module_exit — called when you do: sudo rmmod monitor
 * ──────────────────────────────────────────────────────────── */
static void __exit monitor_exit(void)
{
    struct monitored_container *entry, *tmp;

    /* Stop the timer first */
    timer_shutdown_sync(&check_timer);

    /* Free every entry in the linked list */
    mutex_lock(&list_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_mutex);

    unregister_chrdev(major_number, DEVICE_NAME);
    pr_info("container_monitor: unloaded, all entries freed\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
