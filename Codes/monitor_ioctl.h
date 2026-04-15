#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

/* ──────────────────────────────────────────────
 * monitor_ioctl.h
 *
 * GIVEN BY PROFESSOR (shared "language" file)
 *
 * This file defines the commands used by engine.c (user space)
 * to talk to monitor.c (kernel space) via ioctl.
 *
 * Think of it as a menu of available kernel commands.
 * ────────────────────────────────────────────── */

#define MONITOR_MAGIC  'M'   /* Magic number to identify our device */

/* Structure sent from engine.c → kernel to register a container */
struct container_info {
    pid_t  pid;              /* Host PID of the container process  */
    long   soft_limit_kb;   /* Soft memory limit in kilobytes      */
    long   hard_limit_kb;   /* Hard memory limit in kilobytes      */
    char   name[64];        /* Container name (e.g. "alpha")       */
};

/* ioctl command numbers */
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_info)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, pid_t)
#define MONITOR_LIST       _IOR(MONITOR_MAGIC, 3, int)   /* returns count */

#endif /* MONITOR_IOCTL_H */
