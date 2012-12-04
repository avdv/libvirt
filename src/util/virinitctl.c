/*
 * virinitctl.c: API for talking to init systems via initctl
 *
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <sys/param.h>
#include <fcntl.h>

#include "internal.h"
#include "virinitctl.h"
#include "virterror_internal.h"
#include "util.h"
#include "memory.h"
#include "virfile.h"

#define VIR_FROM_THIS VIR_FROM_INITCTL

/* These constants & struct definitions are taken from
 * systemd, under terms of LGPLv2+
 *
 * initreq.h    Interface to talk to init through /dev/initctl.
 *
 *              Copyright (C) 1995-2004 Miquel van Smoorenburg
 */

#if defined(__FreeBSD_kernel__)
# define VIR_INITCTL_FIFO  "/etc/.initctl"
#else
# define VIR_INITCTL_FIFO  "/dev/initctl"
#endif

#define VIR_INITCTL_MAGIC 0x03091969
#define VIR_INITCTL_CMD_START          0
#define VIR_INITCTL_CMD_RUNLVL         1
#define VIR_INITCTL_CMD_POWERFAIL      2
#define VIR_INITCTL_CMD_POWERFAILNOW   3
#define VIR_INITCTL_CMD_POWEROK        4
#define VIR_INITCTL_CMD_BSD            5
#define VIR_INITCTL_CMD_SETENV         6
#define VIR_INITCTL_CMD_UNSETENV       7

#define VIR_INITCTL_CMD_CHANGECONS     12345

#ifdef MAXHOSTNAMELEN
# define VIR_INITCTL_RQ_HLEN   MAXHOSTNAMELEN
#else
# define VIR_INITCTL_RQ_HLEN   64
#endif

/*
*      This is what BSD 4.4 uses when talking to init.
*      Linux doesn't use this right now.
*/
struct virInitctlRequestBSD {
    char    gen_id[8];              /* Beats me.. telnetd uses "fe" */
    char    tty_id[16];             /* Tty name minus /dev/tty      */
    char    host[VIR_INITCTL_RQ_HLEN]; /* Hostname                     */
    char    term_type[16];          /* Terminal type                */
    int     signal;                 /* Signal to send               */
    int     pid_value;              /* Process to send to           */
    char    exec_name[128];         /* Program to execute           */
    char    reserved[128];          /* For future expansion.        */
};


/*
 *      Because of legacy interfaces, "runlevel" and "sleeptime"
 *      aren't in a separate struct in the union.
 *
 *      The weird sizes are because init expects the whole
 *      struct to be 384 bytes.
 */
struct virInitctlRequest {
    int     magic;                  /* Magic number                 */
    int     cmd;                    /* What kind of request         */
    int     runlevel;               /* Runlevel to change to        */
    int     sleeptime;              /* Time between TERM and KILL   */
    union {
        struct virInitctlRequestBSD bsd;
        char                     data[368];
    } i;
};

verify(sizeof(struct virInitctlRequest) == 384);

/*
 * Send a message to init to change the runlevel
 *
 * Returns 1 on success, 0 if initctl does not exist, -1 on error
 */
int virInitctlSetRunLevel(virInitctlRunLevel level,
                          const char *vroot)
{
    struct virInitctlRequest req;
    int fd = -1;
    char *path = NULL;
    int ret = -1;

    memset(&req, 0, sizeof(req));

    req.magic = VIR_INITCTL_MAGIC;
    req.sleeptime = 0;
    req.cmd = VIR_INITCTL_CMD_RUNLVL;
    /* Yes it is an 'int' field, but wants a numeric character. Go figure */
    req.runlevel = '0' + level;

    if (vroot) {
        if (virAsprintf(&path, "%s/%s", vroot, VIR_INITCTL_FIFO) < 0) {
            virReportOOMError();
            return -1;
        }
    } else {
        if (!(path = strdup(VIR_INITCTL_FIFO))) {
            virReportOOMError();
            return -1;
        }
    }

    if ((fd = open(path, O_WRONLY|O_NONBLOCK|O_CLOEXEC|O_NOCTTY)) < 0) {
        if (errno == ENOENT) {
            ret = 0;
            goto cleanup;
        }
        virReportSystemError(errno,
                             _("Cannot open init control %s"),
                             path);
        goto cleanup;
    }

    if (safewrite(fd, &req, sizeof(req)) != sizeof(req)) {
        virReportSystemError(errno,
                             _("Failed to send request to init control %s"),
                             path);
        goto cleanup;
    }

    ret = 1;

cleanup:
    VIR_FREE(path);
    VIR_FORCE_CLOSE(fd);
    return ret;
}