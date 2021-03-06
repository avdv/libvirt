/*
 * virutil.c: common, generic utility functions
 *
 * Copyright (C) 2006-2013 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 * Copyright (C) 2006, 2007 Binary Karma
 * Copyright (C) 2006 Shuveb Hussain
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 * File created Jul 18, 2007 - Shuveb Hussain <shuveb@binarykarma.com>
 */

#include <config.h>

#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#if HAVE_MMAP
# include <sys/mman.h>
#endif
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <pty.h>
#include <locale.h>

#if HAVE_LIBDEVMAPPER_H
# include <libdevmapper.h>
#endif

#ifdef HAVE_PATHS_H
# include <paths.h>
#endif
#include <netdb.h>
#ifdef HAVE_GETPWUID_R
# include <pwd.h>
# include <grp.h>
#endif
#if WITH_CAPNG
# include <cap-ng.h>
# include <sys/prctl.h>
#endif
#if defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R
# include <mntent.h>
#endif

#ifdef WIN32
# ifdef HAVE_WINSOCK2_H
#  include <winsock2.h>
# endif
# include <windows.h>
# include <shlobj.h>
#endif

#include "c-ctype.h"
#include "dirname.h"
#include "virerror.h"
#include "virlog.h"
#include "virbuffer.h"
#include "virstoragefile.h"
#include "viralloc.h"
#include "virthread.h"
#include "verify.h"
#include "virfile.h"
#include "vircommand.h"
#include "nonblocking.h"
#include "passfd.h"
#include "virprocess.h"
#include "virstring.h"

#ifndef NSIG
# define NSIG 32
#endif

verify(sizeof(gid_t) <= sizeof(unsigned int) &&
       sizeof(uid_t) <= sizeof(unsigned int));

#define VIR_FROM_THIS VIR_FROM_NONE

/* Like read(), but restarts after EINTR.  Doesn't play
 * nicely with nonblocking FD and EAGAIN, in which case
 * you want to use bare read(). Or even use virSocket()
 * if the FD is related to a socket rather than a plain
 * file or pipe. */
ssize_t
saferead(int fd, void *buf, size_t count)
{
    size_t nread = 0;
    while (count > 0) {
        ssize_t r = read(fd, buf, count);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return r;
        if (r == 0)
            return nread;
        buf = (char *)buf + r;
        count -= r;
        nread += r;
    }
    return nread;
}

/* Like write(), but restarts after EINTR. Doesn't play
 * nicely with nonblocking FD and EAGAIN, in which case
 * you want to use bare write(). Or even use virSocket()
 * if the FD is related to a socket rather than a plain
 * file or pipe. */
ssize_t
safewrite(int fd, const void *buf, size_t count)
{
    size_t nwritten = 0;
    while (count > 0) {
        ssize_t r = write(fd, buf, count);

        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return r;
        if (r == 0)
            return nwritten;
        buf = (const char *)buf + r;
        count -= r;
        nwritten += r;
    }
    return nwritten;
}

#ifdef HAVE_POSIX_FALLOCATE
int safezero(int fd, off_t offset, off_t len)
{
    int ret = posix_fallocate(fd, offset, len);
    if (ret == 0)
        return 0;
    errno = ret;
    return -1;
}
#else

# ifdef HAVE_MMAP
int safezero(int fd, off_t offset, off_t len)
{
    int r;
    char *buf;

    /* memset wants the mmap'ed file to be present on disk so create a
     * sparse file
     */
    r = ftruncate(fd, offset + len);
    if (r < 0)
        return -1;

    buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (buf == MAP_FAILED)
        return -1;

    memset(buf, 0, len);
    munmap(buf, len);

    return 0;
}

# else /* HAVE_MMAP */

int safezero(int fd, off_t offset, off_t len)
{
    int r;
    char *buf;
    unsigned long long remain, bytes;

    if (lseek(fd, offset, SEEK_SET) < 0)
        return -1;

    /* Split up the write in small chunks so as not to allocate lots of RAM */
    remain = len;
    bytes = 1024 * 1024;

    r = VIR_ALLOC_N(buf, bytes);
    if (r < 0) {
        errno = ENOMEM;
        return -1;
    }

    while (remain) {
        if (bytes > remain)
            bytes = remain;

        r = safewrite(fd, buf, bytes);
        if (r < 0) {
            VIR_FREE(buf);
            return -1;
        }

        /* safewrite() guarantees all data will be written */
        remain -= bytes;
    }
    VIR_FREE(buf);
    return 0;
}
# endif /* HAVE_MMAP */
#endif /* HAVE_POSIX_FALLOCATE */

int virFileStripSuffix(char *str,
                       const char *suffix)
{
    int len = strlen(str);
    int suffixlen = strlen(suffix);

    if (len < suffixlen)
        return 0;

    if (!STREQ(str + len - suffixlen, suffix))
        return 0;

    str[len-suffixlen] = '\0';

    return 1;
}

#ifndef WIN32

int virSetInherit(int fd, bool inherit) {
    int fflags;
    if ((fflags = fcntl(fd, F_GETFD)) < 0)
        return -1;
    if (inherit)
        fflags &= ~FD_CLOEXEC;
    else
        fflags |= FD_CLOEXEC;
    if ((fcntl(fd, F_SETFD, fflags)) < 0)
        return -1;
    return 0;
}

#else /* WIN32 */

int virSetInherit(int fd ATTRIBUTE_UNUSED, bool inherit ATTRIBUTE_UNUSED)
{
    /* FIXME: Currently creating child processes is not supported on
     * Win32, so there is no point in failing calls that are only relevant
     * when creating child processes. So just pretend that we changed the
     * inheritance property of the given fd as requested. */
    return 0;
}

#endif /* WIN32 */

int virSetBlocking(int fd, bool blocking) {
    return set_nonblocking_flag(fd, !blocking);
}

int virSetNonBlock(int fd) {
    return virSetBlocking(fd, false);
}

int virSetCloseExec(int fd)
{
    return virSetInherit(fd, false);
}

int
virPipeReadUntilEOF(int outfd, int errfd,
                    char **outbuf, char **errbuf) {

    struct pollfd fds[2];
    int i;
    int finished[2];

    fds[0].fd = outfd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    finished[0] = 0;
    fds[1].fd = errfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    finished[1] = 0;

    while (!(finished[0] && finished[1])) {

        if (poll(fds, ARRAY_CARDINALITY(fds), -1) < 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
                continue;
            goto pollerr;
        }

        for (i = 0; i < ARRAY_CARDINALITY(fds); ++i) {
            char data[1024], **buf;
            int got, size;

            if (!(fds[i].revents))
                continue;
            else if (fds[i].revents & POLLHUP)
                finished[i] = 1;

            if (!(fds[i].revents & POLLIN)) {
                if (fds[i].revents & POLLHUP)
                    continue;

                virReportError(VIR_ERR_INTERNAL_ERROR,
                               "%s", _("Unknown poll response."));
                goto error;
            }

            got = read(fds[i].fd, data, sizeof(data));

            if (got == sizeof(data))
                finished[i] = 0;

            if (got == 0) {
                finished[i] = 1;
                continue;
            }
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN)
                    break;
                goto pollerr;
            }

            buf = ((fds[i].fd == outfd) ? outbuf : errbuf);
            size = (*buf ? strlen(*buf) : 0);
            if (VIR_REALLOC_N(*buf, size+got+1) < 0) {
                virReportOOMError();
                goto error;
            }
            memmove(*buf+size, data, got);
            (*buf)[size+got] = '\0';
        }
        continue;

    pollerr:
        virReportSystemError(errno,
                             "%s", _("poll error"));
        goto error;
    }

    return 0;

error:
    VIR_FREE(*outbuf);
    VIR_FREE(*errbuf);
    return -1;
}

/* Like gnulib's fread_file, but read no more than the specified maximum
   number of bytes.  If the length of the input is <= max_len, and
   upon error while reading that data, it works just like fread_file.  */
static char *
saferead_lim(int fd, size_t max_len, size_t *length)
{
    char *buf = NULL;
    size_t alloc = 0;
    size_t size = 0;
    int save_errno;

    for (;;) {
        int count;
        int requested;

        if (size + BUFSIZ + 1 > alloc) {
            alloc += alloc / 2;
            if (alloc < size + BUFSIZ + 1)
                alloc = size + BUFSIZ + 1;

            if (VIR_REALLOC_N(buf, alloc) < 0) {
                save_errno = errno;
                break;
            }
        }

        /* Ensure that (size + requested <= max_len); */
        requested = MIN(size < max_len ? max_len - size : 0,
                        alloc - size - 1);
        count = saferead(fd, buf + size, requested);
        size += count;

        if (count != requested || requested == 0) {
            save_errno = errno;
            if (count < 0)
                break;
            buf[size] = '\0';
            *length = size;
            return buf;
        }
    }

    VIR_FREE(buf);
    errno = save_errno;
    return NULL;
}

/* A wrapper around saferead_lim that maps a failure due to
   exceeding the maximum size limitation to EOVERFLOW.  */
int
virFileReadLimFD(int fd, int maxlen, char **buf)
{
    size_t len;
    char *s;

    if (maxlen <= 0) {
        errno = EINVAL;
        return -1;
    }
    s = saferead_lim(fd, maxlen+1, &len);
    if (s == NULL)
        return -1;
    if (len > maxlen || (int)len != len) {
        VIR_FREE(s);
        /* There was at least one byte more than MAXLEN.
           Set errno accordingly. */
        errno = EOVERFLOW;
        return -1;
    }
    *buf = s;
    return len;
}

int virFileReadAll(const char *path, int maxlen, char **buf)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        virReportSystemError(errno, _("Failed to open file '%s'"), path);
        return -1;
    }

    int len = virFileReadLimFD(fd, maxlen, buf);
    VIR_FORCE_CLOSE(fd);
    if (len < 0) {
        virReportSystemError(errno, _("Failed to read file '%s'"), path);
        return -1;
    }

    return len;
}

/* Truncate @path and write @str to it.  If @mode is 0, ensure that
   @path exists; otherwise, use @mode if @path must be created.
   Return 0 for success, nonzero for failure.
   Be careful to preserve any errno value upon failure. */
int virFileWriteStr(const char *path, const char *str, mode_t mode)
{
    int fd;

    if (mode)
        fd = open(path, O_WRONLY|O_TRUNC|O_CREAT, mode);
    else
        fd = open(path, O_WRONLY|O_TRUNC);
    if (fd == -1)
        return -1;

    if (safewrite(fd, str, strlen(str)) < 0) {
        VIR_FORCE_CLOSE(fd);
        return -1;
    }

    /* Use errno from failed close only if there was no write error.  */
    if (VIR_CLOSE(fd) != 0)
        return -1;

    return 0;
}

int virFileMatchesNameSuffix(const char *file,
                             const char *name,
                             const char *suffix)
{
    int filelen = strlen(file);
    int namelen = strlen(name);
    int suffixlen = strlen(suffix);

    if (filelen == (namelen + suffixlen) &&
        STREQLEN(file, name, namelen) &&
        STREQLEN(file + namelen, suffix, suffixlen))
        return 1;
    else
        return 0;
}

int virFileHasSuffix(const char *str,
                     const char *suffix)
{
    int len = strlen(str);
    int suffixlen = strlen(suffix);

    if (len < suffixlen)
        return 0;

    return STRCASEEQ(str + len - suffixlen, suffix);
}

#define SAME_INODE(Stat_buf_1, Stat_buf_2) \
  ((Stat_buf_1).st_ino == (Stat_buf_2).st_ino \
   && (Stat_buf_1).st_dev == (Stat_buf_2).st_dev)

/* Return nonzero if checkLink and checkDest
   refer to the same file.  Otherwise, return 0.  */
int virFileLinkPointsTo(const char *checkLink,
                        const char *checkDest)
{
    struct stat src_sb;
    struct stat dest_sb;

    return (stat(checkLink, &src_sb) == 0
            && stat(checkDest, &dest_sb) == 0
            && SAME_INODE(src_sb, dest_sb));
}



static int
virFileResolveLinkHelper(const char *linkpath,
                         bool intermediatePaths,
                         char **resultpath)
{
    struct stat st;

    *resultpath = NULL;

    /* We don't need the full canonicalization of intermediate
     * directories, if linkpath is absolute and the basename is
     * already a non-symlink.  */
    if (IS_ABSOLUTE_FILE_NAME(linkpath) && !intermediatePaths) {
        if (lstat(linkpath, &st) < 0)
            return -1;

        if (!S_ISLNK(st.st_mode)) {
            if (!(*resultpath = strdup(linkpath)))
                return -1;
            return 0;
        }
    }

    *resultpath = canonicalize_file_name(linkpath);

    return *resultpath == NULL ? -1 : 0;
}

/*
 * Attempt to resolve a symbolic link, returning an
 * absolute path where only the last component is guaranteed
 * not to be a symlink.
 *
 * Return 0 if path was not a symbolic, or the link was
 * resolved. Return -1 with errno set upon error
 */
int virFileResolveLink(const char *linkpath,
                       char **resultpath)
{
    return virFileResolveLinkHelper(linkpath, false, resultpath);
}

/*
 * Attempt to resolve a symbolic link, returning an
 * absolute path where every component is guaranteed
 * not to be a symlink.
 *
 * Return 0 if path was not a symbolic, or the link was
 * resolved. Return -1 with errno set upon error
 */
int virFileResolveAllLinks(const char *linkpath,
                           char **resultpath)
{
    return virFileResolveLinkHelper(linkpath, true, resultpath);
}

/*
 * Check whether the given file is a link.
 * Returns 1 in case of the file being a link, 0 in case it is not
 * a link and the negative errno in all other cases.
 */
int virFileIsLink(const char *linkpath)
{
    struct stat st;

    if (lstat(linkpath, &st) < 0)
        return -errno;

    return S_ISLNK(st.st_mode) != 0;
}


/*
 * Finds a requested executable file in the PATH env. e.g.:
 * "kvm-img" will return "/usr/bin/kvm-img"
 *
 * You must free the result
 */
char *virFindFileInPath(const char *file)
{
    char *path = NULL;
    char *pathiter;
    char *pathseg;
    char *fullpath = NULL;

    if (file == NULL)
        return NULL;

    /* if we are passed an absolute path (starting with /), return a
     * copy of that path, after validating that it is executable
     */
    if (IS_ABSOLUTE_FILE_NAME(file)) {
        if (virFileIsExecutable(file))
            return strdup(file);
        else
            return NULL;
    }

    /* If we are passed an anchored path (containing a /), then there
     * is no path search - it must exist in the current directory
     */
    if (strchr(file, '/')) {
        if (virFileIsExecutable(file))
            ignore_value(virFileAbsPath(file, &path));
        return path;
    }

    /* copy PATH env so we can tweak it */
    path = getenv("PATH");

    if (path == NULL || (path = strdup(path)) == NULL)
        return NULL;

    /* for each path segment, append the file to search for and test for
     * it. return it if found.
     */
    pathiter = path;
    while ((pathseg = strsep(&pathiter, ":")) != NULL) {
        if (virAsprintf(&fullpath, "%s/%s", pathseg, file) < 0 ||
            virFileIsExecutable(fullpath))
            break;
        VIR_FREE(fullpath);
    }

    VIR_FREE(path);
    return fullpath;
}

bool virFileIsDir(const char *path)
{
    struct stat s;
    return (stat(path, &s) == 0) && S_ISDIR(s.st_mode);
}

bool virFileExists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* Check that a file is regular and has executable bits.  If false is
 * returned, errno is valid.
 *
 * Note: In the presence of ACLs, this may return true for a file that
 * would actually fail with EACCES for a given user, or false for a
 * file that the user could actually execute, but setups with ACLs
 * that weird are unusual. */
bool
virFileIsExecutable(const char *file)
{
    struct stat sb;

    /* We would also want to check faccessat if we cared about ACLs,
     * but we don't.  */
    if (stat(file, &sb) < 0)
        return false;
    if (S_ISREG(sb.st_mode) && (sb.st_mode & 0111) != 0)
        return true;
    errno = S_ISDIR(sb.st_mode) ? EISDIR : EACCES;
    return false;
}

#ifndef WIN32
/* Check that a file is accessible under certain
 * user & gid.
 * @mode can be F_OK, or a bitwise combination of R_OK, W_OK, and X_OK.
 * see 'man access' for more details.
 * Returns 0 on success, -1 on fail with errno set.
 */
int
virFileAccessibleAs(const char *path, int mode,
                    uid_t uid, gid_t gid)
{
    pid_t pid = 0;
    int status, ret = 0;
    int forkRet = 0;

    if (uid == getuid() &&
        gid == getgid())
        return access(path, mode);

    forkRet = virFork(&pid);

    if (pid < 0) {
        return -1;
    }

    if (pid) { /* parent */
        if (virProcessWait(pid, &status) < 0) {
            /* virProcessWait() already
             * reported error */
            return -1;
        }

        if (!WIFEXITED(status)) {
            errno = EINTR;
            return -1;
        }

        if (status) {
            errno = WEXITSTATUS(status);
            return -1;
        }

        return 0;
    }

    /* child.
     * Return positive value here. Parent
     * will change it to negative one. */

    if (forkRet < 0) {
        ret = errno;
        goto childerror;
    }

    if (virSetUIDGID(uid, gid) < 0) {
        ret = errno;
        goto childerror;
    }

    if (access(path, mode) < 0)
        ret = errno;

childerror:
    if ((ret & 0xFF) != ret) {
        VIR_WARN("unable to pass desired return value %d", ret);
        ret = 0xFF;
    }

    _exit(ret);
}

/* virFileOpenForceOwnerMode() - an internal utility function called
 * only by virFileOpenAs().  Sets the owner and mode of the file
 * opened as "fd" if it's not correct AND the flags say it should be
 * forced. */
static int
virFileOpenForceOwnerMode(const char *path, int fd, mode_t mode,
                          uid_t uid, gid_t gid, unsigned int flags)
{
    int ret = 0;
    struct stat st;

    if (!(flags & (VIR_FILE_OPEN_FORCE_OWNER | VIR_FILE_OPEN_FORCE_MODE)))
        return 0;

    if (fstat(fd, &st) == -1) {
        ret = -errno;
        virReportSystemError(errno, _("stat of '%s' failed"), path);
        return ret;
    }
    /* NB: uid:gid are never "-1" (default) at this point - the caller
     * has always changed -1 to the value of get[gu]id().
    */
    if ((flags & VIR_FILE_OPEN_FORCE_OWNER) &&
        ((st.st_uid != uid) || (st.st_gid != gid)) &&
        (fchown(fd, uid, gid) < 0)) {
        ret = -errno;
        virReportSystemError(errno,
                             _("cannot chown '%s' to (%u, %u)"),
                             path, (unsigned int) uid,
                             (unsigned int) gid);
        return ret;
    }
    if ((flags & VIR_FILE_OPEN_FORCE_MODE) &&
        ((mode & (S_IRWXU|S_IRWXG|S_IRWXO)) !=
         (st.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO))) &&
        (fchmod(fd, mode) < 0)) {
        ret = -errno;
        virReportSystemError(errno,
                             _("cannot set mode of '%s' to %04o"),
                             path, mode);
        return ret;
    }
    return ret;
}

/* virFileOpenForked() - an internal utility function called only by
 * virFileOpenAs(). It forks, then the child does setuid+setgid to
 * given uid:gid and attempts to open the file, while the parent just
 * calls recvfd to get the open fd back from the child. returns the
 * fd, or -errno if there is an error. */
static int
virFileOpenForked(const char *path, int openflags, mode_t mode,
                  uid_t uid, gid_t gid, unsigned int flags)
{
    pid_t pid;
    int waitret, status, ret = 0;
    int fd = -1;
    int pair[2] = { -1, -1 };
    int forkRet;

    /* parent is running as root, but caller requested that the
     * file be opened as some other user and/or group). The
     * following dance avoids problems caused by root-squashing
     * NFS servers. */

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        ret = -errno;
        virReportSystemError(errno,
                             _("failed to create socket needed for '%s'"),
                             path);
        return ret;
    }

    forkRet = virFork(&pid);
    if (pid < 0)
        return -errno;

    if (pid == 0) {

        /* child */

        VIR_FORCE_CLOSE(pair[0]); /* preserves errno */
        if (forkRet < 0) {
            /* error encountered and logged in virFork() after the fork. */
            ret = -errno;
            goto childerror;
        }

        /* set desired uid/gid, then attempt to create the file */

        if (virSetUIDGID(uid, gid) < 0) {
            ret = -errno;
            goto childerror;
        }

        if ((fd = open(path, openflags, mode)) < 0) {
            ret = -errno;
            virReportSystemError(errno,
                                 _("child process failed to create file '%s'"),
                                 path);
            goto childerror;
        }

        /* File is successfully open. Set permissions if requested. */
        ret = virFileOpenForceOwnerMode(path, fd, mode, uid, gid, flags);
        if (ret < 0)
            goto childerror;

        do {
            ret = sendfd(pair[1], fd);
        } while (ret < 0 && errno == EINTR);

        if (ret < 0) {
            ret = -errno;
            virReportSystemError(errno, "%s",
                                 _("child process failed to send fd to parent"));
            goto childerror;
        }

    childerror:
        /* ret tracks -errno on failure, but exit value must be positive.
         * If the child exits with EACCES, then the parent tries again.  */
        /* XXX This makes assumptions about errno being < 255, which is
         * not true on Hurd.  */
        VIR_FORCE_CLOSE(pair[1]);
        if (ret < 0) {
            VIR_FORCE_CLOSE(fd);
        }
        ret = -ret;
        if ((ret & 0xff) != ret) {
            VIR_WARN("unable to pass desired return value %d", ret);
            ret = 0xff;
        }
        _exit(ret);
    }

    /* parent */

    VIR_FORCE_CLOSE(pair[1]);

    do {
        fd = recvfd(pair[0], 0);
    } while (fd < 0 && errno == EINTR);
    VIR_FORCE_CLOSE(pair[0]); /* NB: this preserves errno */

    if (fd < 0 && errno != EACCES) {
        ret = -errno;
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR);
        return ret;
    }

    /* wait for child to complete, and retrieve its exit code */
    while ((waitret = waitpid(pid, &status, 0) == -1)
           && (errno == EINTR));
    if (waitret == -1) {
        ret = -errno;
        virReportSystemError(errno,
                             _("failed to wait for child creating '%s'"),
                             path);
        VIR_FORCE_CLOSE(fd);
        return ret;
    }
    if (!WIFEXITED(status) || (ret = -WEXITSTATUS(status)) == -EACCES ||
        fd == -1) {
        /* fall back to the simpler method, which works better in
         * some cases */
        VIR_FORCE_CLOSE(fd);
        if (flags & VIR_FILE_OPEN_NOFORK) {
            /* If we had already tried opening w/o fork+setuid and
             * failed, no sense trying again. Just set return the
             * original errno that we got at that time (by
             * definition, always either EACCES or EPERM - EACCES
             * is close enough).
             */
            return -EACCES;
        }
        if ((fd = open(path, openflags, mode)) < 0)
            return -errno;
        ret = virFileOpenForceOwnerMode(path, fd, mode, uid, gid, flags);
        if (ret < 0) {
            VIR_FORCE_CLOSE(fd);
            return ret;
        }
    }
    return fd;
}

/**
 * virFileOpenAs:
 * @path: file to open or create
 * @openflags: flags to pass to open
 * @mode: mode to use on creation or when forcing permissions
 * @uid: uid that should own file on creation
 * @gid: gid that should own file
 * @flags: bit-wise or of VIR_FILE_OPEN_* flags
 *
 * Open @path, and return an fd to the open file. @openflags contains
 * the flags normally passed to open(2), while those in @flags are
 * used internally. If @flags includes VIR_FILE_OPEN_NOFORK, then try
 * opening the file while executing with the current uid:gid
 * (i.e. don't fork+setuid+setgid before the call to open()).  If
 * @flags includes VIR_FILE_OPEN_FORK, then try opening the file while
 * the effective user id is @uid (by forking a child process); this
 * allows one to bypass root-squashing NFS issues; NOFORK is always
 * tried before FORK (the absence of both flags is treated identically
 * to (VIR_FILE_OPEN_NOFORK | VIR_FILE_OPEN_FORK)). If @flags includes
 * VIR_FILE_OPEN_FORCE_OWNER, then ensure that @path is owned by
 * uid:gid before returning (even if it already existed with a
 * different owner). If @flags includes VIR_FILE_OPEN_FORCE_MODE,
 * ensure it has those permissions before returning (again, even if
 * the file already existed with different permissions).  The return
 * value (if non-negative) is the file descriptor, left open.  Returns
 * -errno on failure.  */
int
virFileOpenAs(const char *path, int openflags, mode_t mode,
              uid_t uid, gid_t gid, unsigned int flags)
{
    int ret = 0, fd = -1;

    /* allow using -1 to mean "current value" */
    if (uid == (uid_t) -1)
        uid = getuid();
    if (gid == (gid_t) -1)
        gid = getgid();

    /* treat absence of both flags as presence of both for simpler
     * calling. */
    if (!(flags & (VIR_FILE_OPEN_NOFORK|VIR_FILE_OPEN_FORK)))
        flags |= VIR_FILE_OPEN_NOFORK|VIR_FILE_OPEN_FORK;

    if ((flags & VIR_FILE_OPEN_NOFORK)
        || (getuid() != 0)
        || ((uid == 0) && (gid == 0))) {

        if ((fd = open(path, openflags, mode)) < 0) {
            ret = -errno;
        } else {
            ret = virFileOpenForceOwnerMode(path, fd, mode, uid, gid, flags);
            if (ret < 0)
                goto error;
        }
    }

    /* If we either 1) didn't try opening as current user at all, or
     * 2) failed, and errno/virStorageFileIsSharedFS indicate we might
     * be successful if we try as a different uid, then try doing
     * fork+setuid+setgid before opening.
     */
    if ((fd < 0) && (flags & VIR_FILE_OPEN_FORK)) {

        if (ret < 0) {
            /* An open(2) that failed due to insufficient permissions
             * could return one or the other of these depending on OS
             * version and circumstances. Any other errno indicates a
             * problem that couldn't be remedied by fork+setuid
             * anyway. */
            if (ret != -EACCES && ret != -EPERM)
                goto error;

            /* On Linux we can also verify the FS-type of the
             * directory.  (this is a NOP on other platforms). */
            switch (virStorageFileIsSharedFS(path)) {
            case 1:
                /* it was on a network share, so we'll re-try */
                break;
            case -1:
                /* failure detecting fstype */
                virReportSystemError(errno, _("couldn't determine fs type "
                                              "of mount containing '%s'"), path);
                goto error;
            case 0:
            default:
                /* file isn't on a recognized network FS */
                goto error;
            }
        }

        /* passed all prerequisites - retry the open w/fork+setuid */
        if ((fd = virFileOpenForked(path, openflags, mode, uid, gid, flags)) < 0) {
            ret = fd;
            fd = -1;
            goto error;
        }
    }

    /* File is successfully opened */

    return fd;

error:
    if (fd < 0) {
        /* whoever failed the open last has already set ret = -errno */
        virReportSystemError(-ret, openflags & O_CREAT
                             ? _("failed to create file '%s'")
                             : _("failed to open file '%s'"),
                             path);
    } else {
        /* some other failure after the open succeeded */
        VIR_FORCE_CLOSE(fd);
    }
    return ret;
}

/* return -errno on failure, or 0 on success */
static int virDirCreateNoFork(const char *path, mode_t mode, uid_t uid, gid_t gid,
                              unsigned int flags) {
    int ret = 0;
    struct stat st;

    if ((mkdir(path, mode) < 0)
        && !((errno == EEXIST) && (flags & VIR_DIR_CREATE_ALLOW_EXIST))) {
        ret = -errno;
        virReportSystemError(errno, _("failed to create directory '%s'"),
                             path);
        goto error;
    }

    if (stat(path, &st) == -1) {
        ret = -errno;
        virReportSystemError(errno, _("stat of '%s' failed"), path);
        goto error;
    }
    if (((st.st_uid != uid) || (st.st_gid != gid))
        && (chown(path, uid, gid) < 0)) {
        ret = -errno;
        virReportSystemError(errno, _("cannot chown '%s' to (%u, %u)"),
                             path, (unsigned int) uid, (unsigned int) gid);
        goto error;
    }
    if ((flags & VIR_DIR_CREATE_FORCE_PERMS)
        && (chmod(path, mode) < 0)) {
        ret = -errno;
        virReportSystemError(errno,
                             _("cannot set mode of '%s' to %04o"),
                             path, mode);
        goto error;
    }
error:
    return ret;
}

/* return -errno on failure, or 0 on success */
int virDirCreate(const char *path, mode_t mode,
                 uid_t uid, gid_t gid, unsigned int flags) {
    struct stat st;
    pid_t pid;
    int waitret;
    int status, ret = 0;

    /* allow using -1 to mean "current value" */
    if (uid == (uid_t) -1)
        uid = getuid();
    if (gid == (gid_t) -1)
        gid = getgid();

    if ((!(flags & VIR_DIR_CREATE_AS_UID))
        || (getuid() != 0)
        || ((uid == 0) && (gid == 0))
        || ((flags & VIR_DIR_CREATE_ALLOW_EXIST) && (stat(path, &st) >= 0))) {
        return virDirCreateNoFork(path, mode, uid, gid, flags);
    }

    int forkRet = virFork(&pid);

    if (pid < 0) {
        ret = -errno;
        return ret;
    }

    if (pid) { /* parent */
        /* wait for child to complete, and retrieve its exit code */
        while ((waitret = waitpid(pid, &status, 0) == -1)  && (errno == EINTR));
        if (waitret == -1) {
            ret = -errno;
            virReportSystemError(errno,
                                 _("failed to wait for child creating '%s'"),
                                 path);
            goto parenterror;
        }
        if (!WIFEXITED(status) || (ret = -WEXITSTATUS(status)) == -EACCES) {
            /* fall back to the simpler method, which works better in
             * some cases */
            return virDirCreateNoFork(path, mode, uid, gid, flags);
        }
parenterror:
        return ret;
    }

    /* child */

    if (forkRet < 0) {
        /* error encountered and logged in virFork() after the fork. */
        goto childerror;
    }

    /* set desired uid/gid, then attempt to create the directory */

    if (virSetUIDGID(uid, gid) < 0) {
        ret = -errno;
        goto childerror;
    }
    if (mkdir(path, mode) < 0) {
        ret = -errno;
        if (ret != -EACCES) {
            /* in case of EACCES, the parent will retry */
            virReportSystemError(errno, _("child failed to create directory '%s'"),
                                 path);
        }
        goto childerror;
    }
    /* check if group was set properly by creating after
     * setgid. If not, try doing it with chown */
    if (stat(path, &st) == -1) {
        ret = -errno;
        virReportSystemError(errno,
                             _("stat of '%s' failed"), path);
        goto childerror;
    }
    if ((st.st_gid != gid) && (chown(path, (uid_t) -1, gid) < 0)) {
        ret = -errno;
        virReportSystemError(errno,
                             _("cannot chown '%s' to group %u"),
                             path, (unsigned int) gid);
        goto childerror;
    }
    if ((flags & VIR_DIR_CREATE_FORCE_PERMS)
        && chmod(path, mode) < 0) {
        virReportSystemError(errno,
                             _("cannot set mode of '%s' to %04o"),
                             path, mode);
        goto childerror;
    }
childerror:
    _exit(ret);
}

#else /* WIN32 */

int
virFileAccessibleAs(const char *path,
                    int mode,
                    uid_t uid ATTRIBUTE_UNUSED,
                    gid_t gid ATTRIBUTE_UNUSED)
{

    VIR_WARN("Ignoring uid/gid due to WIN32");

    return access(path, mode);
}

/* return -errno on failure, or 0 on success */
int virFileOpenAs(const char *path ATTRIBUTE_UNUSED,
                  int openflags ATTRIBUTE_UNUSED,
                  mode_t mode ATTRIBUTE_UNUSED,
                  uid_t uid ATTRIBUTE_UNUSED,
                  gid_t gid ATTRIBUTE_UNUSED,
                  unsigned int flags_unused ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virFileOpenAs is not implemented for WIN32"));

    return -ENOSYS;
}

int virDirCreate(const char *path ATTRIBUTE_UNUSED,
                 mode_t mode ATTRIBUTE_UNUSED,
                 uid_t uid ATTRIBUTE_UNUSED,
                 gid_t gid ATTRIBUTE_UNUSED,
                 unsigned int flags_unused ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virDirCreate is not implemented for WIN32"));

    return -ENOSYS;
}
#endif /* WIN32 */

static int virFileMakePathHelper(char *path, mode_t mode)
{
    struct stat st;
    char *p;

    VIR_DEBUG("path=%s mode=0%o", path, mode);

    if (stat(path, &st) >= 0) {
        if (S_ISDIR(st.st_mode))
            return 0;

        errno = ENOTDIR;
        return -1;
    }

    if (errno != ENOENT)
        return -1;

    if ((p = strrchr(path, '/')) == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (p != path) {
        *p = '\0';

        if (virFileMakePathHelper(path, mode) < 0)
            return -1;

        *p = '/';
    }

    if (mkdir(path, mode) < 0 && errno != EEXIST)
        return -1;

    return 0;
}

/**
 * Creates the given directory with mode 0777 if it's not already existing.
 *
 * Returns 0 on success, or -1 if an error occurred (in which case, errno
 * is set appropriately).
 */
int virFileMakePath(const char *path)
{
    return virFileMakePathWithMode(path, 0777);
}

int
virFileMakePathWithMode(const char *path,
                        mode_t mode)
{
    int ret = -1;
    char *tmp;

    if ((tmp = strdup(path)) == NULL)
        goto cleanup;

    ret = virFileMakePathHelper(tmp, mode);

cleanup:
    VIR_FREE(tmp);
    return ret;
}

/* Build up a fully qualified path for a config file to be
 * associated with a persistent guest or network */
char *
virFileBuildPath(const char *dir, const char *name, const char *ext)
{
    char *path;

    if (ext == NULL) {
        if (virAsprintf(&path, "%s/%s", dir, name) < 0) {
            virReportOOMError();
            return NULL;
        }
    } else {
        if (virAsprintf(&path, "%s/%s%s", dir, name, ext) < 0) {
            virReportOOMError();
            return NULL;
        }
    }

    return path;
}

/* Open a non-blocking master side of a pty.  If ttyName is not NULL,
 * then populate it with the name of the slave.  If rawmode is set,
 * also put the master side into raw mode before returning.  */
#ifndef WIN32
int virFileOpenTty(int *ttymaster,
                   char **ttyName,
                   int rawmode)
{
    /* XXX A word of caution - on some platforms (Solaris and HP-UX),
     * additional ioctl() calls are needs after opening the slave
     * before it will cause isatty() to return true.  Should we make
     * virFileOpenTty also return the opened slave fd, so the caller
     * doesn't have to worry about that mess?  */
    int ret = -1;
    int slave = -1;
    char *name = NULL;

    /* Unfortunately, we can't use the name argument of openpty, since
     * there is no guarantee on how large the buffer has to be.
     * Likewise, we can't use the termios argument: we have to use
     * read-modify-write since there is no portable way to initialize
     * a struct termios without use of tcgetattr.  */
    if (openpty(ttymaster, &slave, NULL, NULL, NULL) < 0)
        return -1;

    /* What a shame that openpty cannot atomically set FD_CLOEXEC, but
     * that using posix_openpt/grantpt/unlockpt/ptsname is not
     * thread-safe, and that ptsname_r is not portable.  */
    if (virSetNonBlock(*ttymaster) < 0 ||
        virSetCloseExec(*ttymaster) < 0)
        goto cleanup;

    /* While Linux supports tcgetattr on either the master or the
     * slave, Solaris requires it to be on the slave.  */
    if (rawmode) {
        struct termios ttyAttr;
        if (tcgetattr(slave, &ttyAttr) < 0)
            goto cleanup;

        cfmakeraw(&ttyAttr);

        if (tcsetattr(slave, TCSADRAIN, &ttyAttr) < 0)
            goto cleanup;
    }

    /* ttyname_r on the slave is required by POSIX, while ptsname_r on
     * the master is a glibc extension, and the POSIX ptsname is not
     * thread-safe.  Since openpty gave us both descriptors, guess
     * which way we will determine the name?  :)  */
    if (ttyName) {
        /* Initial guess of 64 is generally sufficient; rely on ERANGE
         * to tell us if we need to grow.  */
        size_t len = 64;
        int rc;

        if (VIR_ALLOC_N(name, len) < 0)
            goto cleanup;

        while ((rc = ttyname_r(slave, name, len)) == ERANGE) {
            if (VIR_RESIZE_N(name, len, len, len) < 0)
                goto cleanup;
        }
        if (rc != 0) {
            errno = rc;
            goto cleanup;
        }
        *ttyName = name;
        name = NULL;
    }

    ret = 0;

cleanup:
    if (ret != 0)
        VIR_FORCE_CLOSE(*ttymaster);
    VIR_FORCE_CLOSE(slave);
    VIR_FREE(name);

    return ret;
}
#else /* WIN32 */
int virFileOpenTty(int *ttymaster ATTRIBUTE_UNUSED,
                   char **ttyName ATTRIBUTE_UNUSED,
                   int rawmode ATTRIBUTE_UNUSED)
{
    /* mingw completely lacks pseudo-terminals, and the gnulib
     * replacements are not (yet) license compatible.  */
    errno = ENOSYS;
    return -1;
}
#endif /* WIN32 */

bool virFileIsAbsPath(const char *path)
{
    if (!path)
        return false;

    if (VIR_FILE_IS_DIR_SEPARATOR(path[0]))
        return true;

#ifdef WIN32
    if (c_isalpha(path[0]) &&
        path[1] == ':' &&
        VIR_FILE_IS_DIR_SEPARATOR(path[2]))
        return true;
#endif

    return false;
}


const char *virFileSkipRoot(const char *path)
{
#ifdef WIN32
    /* Skip \\server\share or //server/share */
    if (VIR_FILE_IS_DIR_SEPARATOR(path[0]) &&
        VIR_FILE_IS_DIR_SEPARATOR(path[1]) &&
        path[2] &&
        !VIR_FILE_IS_DIR_SEPARATOR(path[2]))
    {
        const char *p = strchr(path + 2, VIR_FILE_DIR_SEPARATOR);
        const char *q = strchr(path + 2, '/');

        if (p == NULL || (q != NULL && q < p))
            p = q;

        if (p && p > path + 2 && p[1]) {
            path = p + 1;

            while (path[0] &&
                   !VIR_FILE_IS_DIR_SEPARATOR(path[0]))
                path++;

            /* Possibly skip a backslash after the share name */
            if (VIR_FILE_IS_DIR_SEPARATOR(path[0]))
                path++;

            return path;
        }
    }
#endif

    /* Skip initial slashes */
    if (VIR_FILE_IS_DIR_SEPARATOR(path[0])) {
        while (VIR_FILE_IS_DIR_SEPARATOR(path[0]))
            path++;

        return path;
    }

#ifdef WIN32
    /* Skip X:\ */
    if (c_isalpha(path[0]) &&
        path[1] == ':' &&
        VIR_FILE_IS_DIR_SEPARATOR(path[2]))
        return path + 3;
#endif

    return path;
}



/*
 * Creates an absolute path for a potentially relative path.
 * Return 0 if the path was not relative, or on success.
 * Return -1 on error.
 *
 * You must free the result.
 */
int virFileAbsPath(const char *path, char **abspath)
{
    char *buf;

    if (path[0] == '/') {
        if (!(*abspath = strdup(path)))
            return -1;
    } else {
        buf = getcwd(NULL, 0);
        if (buf == NULL)
            return -1;

        if (virAsprintf(abspath, "%s/%s", buf, path) < 0) {
            VIR_FREE(buf);
            return -1;
        }
        VIR_FREE(buf);
    }

    return 0;
}

/* Remove spurious / characters from a path. The result must be freed */
char *
virFileSanitizePath(const char *path)
{
    const char *cur = path;
    char *cleanpath;
    int idx = 0;

    cleanpath = strdup(path);
    if (!cleanpath) {
        virReportOOMError();
        return NULL;
    }

    /* Need to sanitize:
     * //           -> //
     * ///          -> /
     * /../foo      -> /../foo
     * /foo///bar/  -> /foo/bar
     */

    /* Starting with // is valid posix, but ///foo == /foo */
    if (cur[0] == '/' && cur[1] == '/' && cur[2] != '/') {
        idx = 2;
        cur += 2;
    }

    /* Sanitize path in place */
    while (*cur != '\0') {
        if (*cur != '/') {
            cleanpath[idx++] = *cur++;
            continue;
        }

        /* Skip all extra / */
        while (*++cur == '/')
            continue;

        /* Don't add a trailing / */
        if (idx != 0 && *cur == '\0')
            break;

        cleanpath[idx++] = '/';
    }
    cleanpath[idx] = '\0';

    return cleanpath;
}

/* Convert C from hexadecimal character to integer.  */
int
virHexToBin(unsigned char c)
{
    switch (c) {
    default: return c - '0';
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    }
}

/* Scale an integer VALUE in-place by an optional case-insensitive
 * SUFFIX, defaulting to SCALE if suffix is NULL or empty (scale is
 * typically 1 or 1024).  Recognized suffixes include 'b' or 'bytes',
 * as well as power-of-two scaling via binary abbreviations ('KiB',
 * 'MiB', ...) or their one-letter counterpart ('k', 'M', ...), and
 * power-of-ten scaling via SI abbreviations ('KB', 'MB', ...).
 * Ensure that the result does not exceed LIMIT.  Return 0 on success,
 * -1 with error message raised on failure.  */
int
virScaleInteger(unsigned long long *value, const char *suffix,
                unsigned long long scale, unsigned long long limit)
{
    if (!suffix || !*suffix) {
        if (!scale) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("invalid scale %llu"), scale);
            return -1;
        }
        suffix = "";
    } else if (STRCASEEQ(suffix, "b") || STRCASEEQ(suffix, "byte") ||
               STRCASEEQ(suffix, "bytes")) {
        scale = 1;
    } else {
        int base;

        if (!suffix[1] || STRCASEEQ(suffix + 1, "iB")) {
            base = 1024;
        } else if (c_tolower(suffix[1]) == 'b' && !suffix[2]) {
            base = 1000;
        } else {
            virReportError(VIR_ERR_INVALID_ARG,
                         _("unknown suffix '%s'"), suffix);
            return -1;
        }
        scale = 1;
        switch (c_tolower(*suffix)) {
        case 'e':
            scale *= base;
            /* fallthrough */
        case 'p':
            scale *= base;
            /* fallthrough */
        case 't':
            scale *= base;
            /* fallthrough */
        case 'g':
            scale *= base;
            /* fallthrough */
        case 'm':
            scale *= base;
            /* fallthrough */
        case 'k':
            scale *= base;
            break;
        default:
            virReportError(VIR_ERR_INVALID_ARG,
                           _("unknown suffix '%s'"), suffix);
            return -1;
        }
    }

    if (*value && *value > (limit / scale)) {
        virReportError(VIR_ERR_OVERFLOW, _("value too large: %llu%s"),
                       *value, suffix);
        return -1;
    }
    *value *= scale;
    return 0;
}


/**
 * virParseNumber:
 * @str: pointer to the char pointer used
 *
 * Parse an unsigned number
 *
 * Returns the unsigned number or -1 in case of error. @str will be
 *         updated to skip the number.
 */
int
virParseNumber(const char **str)
{
    int ret = 0;
    const char *cur = *str;

    if ((*cur < '0') || (*cur > '9'))
        return -1;

    while (c_isdigit(*cur)) {
        unsigned int c = *cur - '0';

        if ((ret > INT_MAX / 10) ||
            ((ret == INT_MAX / 10) && (c > INT_MAX % 10)))
            return -1;
        ret = ret * 10 + c;
        cur++;
    }
    *str = cur;
    return ret;
}

/**
 * virParseVersionString:
 * @str: const char pointer to the version string
 * @version: unsigned long pointer to output the version number
 * @allowMissing: true to treat 3 like 3.0.0, false to error out on
 * missing minor or micro
 *
 * Parse an unsigned version number from a version string. Expecting
 * 'major.minor.micro' format, ignoring an optional suffix.
 *
 * The major, minor and micro numbers are encoded into a single version number:
 *
 *   1000000 * major + 1000 * minor + micro
 *
 * Returns the 0 for success, -1 for error.
 */
int
virParseVersionString(const char *str, unsigned long *version,
                      bool allowMissing)
{
    unsigned int major, minor = 0, micro = 0;
    char *tmp;

    if (virStrToLong_ui(str, &tmp, 10, &major) < 0)
        return -1;

    if (!allowMissing && *tmp != '.')
        return -1;

    if ((*tmp == '.') && virStrToLong_ui(tmp + 1, &tmp, 10, &minor) < 0)
        return -1;

    if (!allowMissing && *tmp != '.')
        return -1;

    if ((*tmp == '.') && virStrToLong_ui(tmp + 1, &tmp, 10, &micro) < 0)
        return -1;

    if (major > UINT_MAX / 1000000 || minor > 999 || micro > 999)
        return -1;

    *version = 1000000 * major + 1000 * minor + micro;

    return 0;
}

int virEnumFromString(const char *const*types,
                      unsigned int ntypes,
                      const char *type)
{
    unsigned int i;
    if (!type)
        return -1;

    for (i = 0 ; i < ntypes ; i++)
        if (STREQ(types[i], type))
            return i;

    return -1;
}

/* In case thread-safe locales are available */
#if HAVE_NEWLOCALE

static locale_t virLocale;

static int
virLocaleOnceInit(void)
{
    virLocale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    if (!virLocale)
        return -1;
    return 0;
}

VIR_ONCE_GLOBAL_INIT(virLocale)
#endif

/**
 * virDoubleToStr
 *
 * converts double to string with C locale (thread-safe).
 *
 * Returns -1 on error, size of the string otherwise.
 */
int
virDoubleToStr(char **strp, double number)
{
    int ret = -1;

#if HAVE_NEWLOCALE

    locale_t old_loc;

    if (virLocaleInitialize() < 0)
        goto error;

    old_loc = uselocale(virLocale);
    ret = virAsprintf(strp, "%lf", number);
    uselocale(old_loc);

#else

    char *radix, *tmp;
    struct lconv *lc;

    if ((ret = virAsprintf(strp, "%lf", number) < 0))
        goto error;

    lc = localeconv();
    radix = lc->decimal_point;
    tmp = strstr(*strp, radix);
    if (tmp) {
        *tmp = '.';
        if (strlen(radix) > 1)
            memmove(tmp + 1, tmp + strlen(radix), strlen(*strp) - (tmp - *strp));
    }

#endif /* HAVE_NEWLOCALE */
 error:
    return ret;
}


/**
 * Format @val as a base-10 decimal number, in the
 * buffer @buf of size @buflen. To allocate a suitable
 * sized buffer, the INT_BUFLEN(int) macro should be
 * used
 *
 * Returns pointer to start of the number in @buf
 */
char *
virFormatIntDecimal(char *buf, size_t buflen, int val)
{
    char *p = buf + buflen - 1;
    *p = '\0';
    if (val >= 0) {
        do {
            *--p = '0' + (val % 10);
            val /= 10;
        } while (val != 0);
    } else {
        do {
            *--p = '0' - (val % 10);
            val /= 10;
        } while (val != 0);
        *--p = '-';
    }
    return p;
}


const char *virEnumToString(const char *const*types,
                            unsigned int ntypes,
                            int type)
{
    if (type < 0 || type >= ntypes)
        return NULL;

    return types[type];
}

/* Translates a device name of the form (regex) /^[fhv]d[a-z]+[0-9]*$/
 * into the corresponding index (e.g. sda => 0, hdz => 25, vdaa => 26)
 * Note that any trailing string of digits is simply ignored.
 * @param name The name of the device
 * @return name's index, or -1 on failure
 */
int virDiskNameToIndex(const char *name) {
    const char *ptr = NULL;
    int idx = 0;
    static char const* const drive_prefix[] = {"fd", "hd", "vd", "sd", "xvd", "ubd"};
    unsigned int i;

    for (i = 0; i < ARRAY_CARDINALITY(drive_prefix); i++) {
        if (STRPREFIX(name, drive_prefix[i])) {
            ptr = name + strlen(drive_prefix[i]);
            break;
        }
    }

    if (!ptr)
        return -1;

    for (i = 0; *ptr; i++) {
        if (!c_islower(*ptr))
            break;

        idx = (idx + (i < 1 ? 0 : 1)) * 26;
        idx += *ptr - 'a';
        ptr++;
    }

    /* Count the trailing digits.  */
    size_t n_digits = strspn(ptr, "0123456789");
    if (ptr[n_digits] != '\0')
        return -1;

    return idx;
}

char *virIndexToDiskName(int idx, const char *prefix)
{
    char *name = NULL;
    int i, k, offset;

    if (idx < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Disk index %d is negative"), idx);
        return NULL;
    }

    for (i = 0, k = idx; k >= 0; ++i, k = k / 26 - 1) { }

    offset = strlen(prefix);

    if (VIR_ALLOC_N(name, offset + i + 1)) {
        virReportOOMError();
        return NULL;
    }

    strcpy(name, prefix);
    name[offset + i] = '\0';

    for (i = i - 1, k = idx; k >= 0; --i, k = k / 26 - 1) {
        name[offset + i] = 'a' + (k % 26);
    }

    return name;
}

#ifndef AI_CANONIDN
# define AI_CANONIDN 0
#endif

/* Who knew getting a hostname could be so delicate.  In Linux (and Unices
 * in general), many things depend on "hostname" returning a value that will
 * resolve one way or another.  In the modern world where networks frequently
 * come and go this is often being hard-coded to resolve to "localhost".  If
 * it *doesn't* resolve to localhost, then we would prefer to have the FQDN.
 * That leads us to 3 possibilities:
 *
 * 1)  gethostname() returns an FQDN (not localhost) - we return the string
 *     as-is, it's all of the information we want
 * 2)  gethostname() returns "localhost" - we return localhost; doing further
 *     work to try to resolve it is pointless
 * 3)  gethostname() returns a shortened hostname - in this case, we want to
 *     try to resolve this to a fully-qualified name.  Therefore we pass it
 *     to getaddrinfo().  There are two possible responses:
 *     a)  getaddrinfo() resolves to a FQDN - return the FQDN
 *     b)  getaddrinfo() fails or resolves to localhost - in this case, the
 *         data we got from gethostname() is actually more useful than what
 *         we got from getaddrinfo().  Return the value from gethostname()
 *         and hope for the best.
 */
char *virGetHostname(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    int r;
    char hostname[HOST_NAME_MAX+1], *result;
    struct addrinfo hints, *info;

    r = gethostname(hostname, sizeof(hostname));
    if (r == -1) {
        virReportSystemError(errno,
                             "%s", _("failed to determine host name"));
        return NULL;
    }
    NUL_TERMINATE(hostname);

    if (STRPREFIX(hostname, "localhost") || strchr(hostname, '.')) {
        /* in this case, gethostname returned localhost (meaning we can't
         * do any further canonicalization), or it returned an FQDN (and
         * we don't need to do any further canonicalization).  Return the
         * string as-is; it's up to callers to check whether "localhost"
         * is allowed.
         */
        result = strdup(hostname);
        goto check_and_return;
    }

    /* otherwise, it's a shortened, non-localhost, hostname.  Attempt to
     * canonicalize the hostname by running it through getaddrinfo
     */

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_CANONNAME|AI_CANONIDN;
    hints.ai_family = AF_UNSPEC;
    r = getaddrinfo(hostname, NULL, &hints, &info);
    if (r != 0) {
        VIR_WARN("getaddrinfo failed for '%s': %s",
                 hostname, gai_strerror(r));
        result = strdup(hostname);
        goto check_and_return;
    }

    /* Tell static analyzers about getaddrinfo semantics.  */
    sa_assert(info);

    if (info->ai_canonname == NULL ||
        STRPREFIX(info->ai_canonname, "localhost"))
        /* in this case, we tried to canonicalize and we ended up back with
         * localhost.  Ignore the canonicalized name and just return the
         * original hostname
         */
        result = strdup(hostname);
    else
        /* Caller frees this string. */
        result = strdup(info->ai_canonname);

    freeaddrinfo(info);

check_and_return:
    if (result == NULL)
        virReportOOMError();
    return result;
}

#ifdef HAVE_GETPWUID_R
enum {
    VIR_USER_ENT_DIRECTORY,
    VIR_USER_ENT_NAME,
};

static char *virGetUserEnt(uid_t uid,
                           int field)
{
    char *strbuf;
    char *ret;
    struct passwd pwbuf;
    struct passwd *pw = NULL;
    long val = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t strbuflen = val;
    int rc;

    /* sysconf is a hint; if it fails, fall back to a reasonable size */
    if (val < 0)
        strbuflen = 1024;

    if (VIR_ALLOC_N(strbuf, strbuflen) < 0) {
        virReportOOMError();
        return NULL;
    }

    /*
     * From the manpage (terrifying but true):
     *
     * ERRORS
     *  0 or ENOENT or ESRCH or EBADF or EPERM or ...
     *        The given name or uid was not found.
     */
    while ((rc = getpwuid_r(uid, &pwbuf, strbuf, strbuflen, &pw)) == ERANGE) {
        if (VIR_RESIZE_N(strbuf, strbuflen, strbuflen, strbuflen) < 0) {
            virReportOOMError();
            VIR_FREE(strbuf);
            return NULL;
        }
    }
    if (rc != 0 || pw == NULL) {
        virReportSystemError(rc,
                             _("Failed to find user record for uid '%u'"),
                             (unsigned int) uid);
        VIR_FREE(strbuf);
        return NULL;
    }

    if (field == VIR_USER_ENT_DIRECTORY)
        ret = strdup(pw->pw_dir);
    else
        ret = strdup(pw->pw_name);

    VIR_FREE(strbuf);
    if (!ret)
        virReportOOMError();

    return ret;
}

static char *virGetGroupEnt(gid_t gid)
{
    char *strbuf;
    char *ret;
    struct group grbuf;
    struct group *gr = NULL;
    long val = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t strbuflen = val;
    int rc;

    /* sysconf is a hint; if it fails, fall back to a reasonable size */
    if (val < 0)
        strbuflen = 1024;

    if (VIR_ALLOC_N(strbuf, strbuflen) < 0) {
        virReportOOMError();
        return NULL;
    }

    /*
     * From the manpage (terrifying but true):
     *
     * ERRORS
     *  0 or ENOENT or ESRCH or EBADF or EPERM or ...
     *        The given name or gid was not found.
     */
    while ((rc = getgrgid_r(gid, &grbuf, strbuf, strbuflen, &gr)) == ERANGE) {
        if (VIR_RESIZE_N(strbuf, strbuflen, strbuflen, strbuflen) < 0) {
            virReportOOMError();
            VIR_FREE(strbuf);
            return NULL;
        }
    }
    if (rc != 0 || gr == NULL) {
        virReportSystemError(rc,
                             _("Failed to find group record for gid '%u'"),
                             (unsigned int) gid);
        VIR_FREE(strbuf);
        return NULL;
    }

    ret = strdup(gr->gr_name);

    VIR_FREE(strbuf);
    if (!ret)
        virReportOOMError();

    return ret;
}

char *virGetUserDirectory(void)
{
    return virGetUserEnt(geteuid(), VIR_USER_ENT_DIRECTORY);
}

static char *virGetXDGDirectory(const char *xdgenvname, const char *xdgdefdir)
{
    const char *path = getenv(xdgenvname);
    char *ret = NULL;
    char *home = virGetUserEnt(geteuid(), VIR_USER_ENT_DIRECTORY);

    if (path && path[0]) {
        if (virAsprintf(&ret, "%s/libvirt", path) < 0)
            goto no_memory;
    } else {
        if (virAsprintf(&ret, "%s/%s/libvirt", home, xdgdefdir) < 0)
            goto no_memory;
    }

 cleanup:
    VIR_FREE(home);
    return ret;
 no_memory:
    virReportOOMError();
    goto cleanup;
}

char *virGetUserConfigDirectory(void)
{
    return virGetXDGDirectory("XDG_CONFIG_HOME", ".config");
}

char *virGetUserCacheDirectory(void)
{
     return virGetXDGDirectory("XDG_CACHE_HOME", ".cache");
}

char *virGetUserRuntimeDirectory(void)
{
    const char *path = getenv("XDG_RUNTIME_DIR");

    if (!path || !path[0]) {
        return virGetUserCacheDirectory();
    } else {
        char *ret;

        if (virAsprintf(&ret, "%s/libvirt", path) < 0) {
            virReportOOMError();
            return NULL;
        }

        return ret;
    }
}

char *virGetUserName(uid_t uid)
{
    return virGetUserEnt(uid, VIR_USER_ENT_NAME);
}

char *virGetGroupName(gid_t gid)
{
    return virGetGroupEnt(gid);
}

/* Search in the password database for a user id that matches the user name
 * `name`. Returns 0 on success, -1 on failure or 1 if name cannot be found.
 */
static int
virGetUserIDByName(const char *name, uid_t *uid)
{
    char *strbuf = NULL;
    struct passwd pwbuf;
    struct passwd *pw = NULL;
    long val = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t strbuflen = val;
    int rc;
    int ret = -1;

    /* sysconf is a hint; if it fails, fall back to a reasonable size */
    if (val < 0)
        strbuflen = 1024;

    if (VIR_ALLOC_N(strbuf, strbuflen) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    while ((rc = getpwnam_r(name, &pwbuf, strbuf, strbuflen, &pw)) == ERANGE) {
        if (VIR_RESIZE_N(strbuf, strbuflen, strbuflen, strbuflen) < 0) {
            virReportOOMError();
            goto cleanup;
        }
    }

    if (!pw) {
        if (rc != 0) {
            char buf[1024];
            /* log the possible error from getpwnam_r. Unfortunately error
             * reporting from this function is bad and we can't really
             * rely on it, so we just report that the user wasn't found */
            VIR_WARN("User record for user '%s' was not found: %s",
                     name, virStrerror(rc, buf, sizeof(buf)));
        }

        ret = 1;
        goto cleanup;
    }

    *uid = pw->pw_uid;
    ret = 0;

cleanup:
    VIR_FREE(strbuf);

    return ret;
}

/* Try to match a user id based on `user`. The default behavior is to parse
 * `user` first as a user name and then as a user id. However if `user`
 * contains a leading '+', the rest of the string is always parsed as a uid.
 *
 * Returns 0 on success and -1 otherwise.
 */
int
virGetUserID(const char *user, uid_t *uid)
{
    unsigned int uint_uid;

    if (*user == '+') {
        user++;
    } else {
        int rc = virGetUserIDByName(user, uid);
        if (rc <= 0)
            return rc;
    }

    if (virStrToLong_ui(user, NULL, 10, &uint_uid) < 0 ||
        ((uid_t) uint_uid) != uint_uid) {
        virReportError(VIR_ERR_INVALID_ARG, _("Failed to parse user '%s'"),
                       user);
        return -1;
    }

    *uid = uint_uid;

    return 0;
}

/* Search in the group database for a group id that matches the group name
 * `name`. Returns 0 on success, -1 on failure or 1 if name cannot be found.
 */
static int
virGetGroupIDByName(const char *name, gid_t *gid)
{
    char *strbuf = NULL;
    struct group grbuf;
    struct group *gr = NULL;
    long val = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t strbuflen = val;
    int rc;
    int ret = -1;

    /* sysconf is a hint; if it fails, fall back to a reasonable size */
    if (val < 0)
        strbuflen = 1024;

    if (VIR_ALLOC_N(strbuf, strbuflen) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    while ((rc = getgrnam_r(name, &grbuf, strbuf, strbuflen, &gr)) == ERANGE) {
        if (VIR_RESIZE_N(strbuf, strbuflen, strbuflen, strbuflen) < 0) {
            virReportOOMError();
            goto cleanup;
        }
    }

    if (!gr) {
        if (rc != 0) {
            char buf[1024];
            /* log the possible error from getgrnam_r. Unfortunately error
             * reporting from this function is bad and we can't really
             * rely on it, so we just report that the user wasn't found */
            VIR_WARN("Group record for user '%s' was not found: %s",
                     name, virStrerror(rc, buf, sizeof(buf)));
        }

        ret = 1;
        goto cleanup;
    }

    *gid = gr->gr_gid;
    ret = 0;

cleanup:
    VIR_FREE(strbuf);

    return ret;
}

/* Try to match a group id based on `group`. The default behavior is to parse
 * `group` first as a group name and then as a group id. However if `group`
 * contains a leading '+', the rest of the string is always parsed as a guid.
 *
 * Returns 0 on success and -1 otherwise.
 */
int
virGetGroupID(const char *group, gid_t *gid)
{
    unsigned int uint_gid;

    if (*group == '+') {
        group++;
    } else {
        int rc = virGetGroupIDByName(group, gid);
        if (rc <= 0)
            return rc;
    }

    if (virStrToLong_ui(group, NULL, 10, &uint_gid) < 0 ||
        ((gid_t) uint_gid) != uint_gid) {
        virReportError(VIR_ERR_INVALID_ARG, _("Failed to parse group '%s'"),
                       group);
        return -1;
    }

    *gid = uint_gid;

    return 0;
}

/* Set the real and effective uid and gid to the given values, and call
 * initgroups so that the process has all the assumed group membership of
 * that uid. return 0 on success, -1 on failure (the original system error
 * remains in errno).
 */
int
virSetUIDGID(uid_t uid, gid_t gid)
{
    int err;
    char *buf = NULL;

    if (gid != (gid_t)-1) {
        if (setregid(gid, gid) < 0) {
            virReportSystemError(err = errno,
                                 _("cannot change to '%u' group"),
                                 (unsigned int) gid);
            goto error;
        }
    }

    if (uid != (uid_t)-1) {
# ifdef HAVE_INITGROUPS
        struct passwd pwd, *pwd_result;
        size_t bufsize;
        int rc;

        bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize == -1)
            bufsize = 16384;

        if (VIR_ALLOC_N(buf, bufsize) < 0) {
            virReportOOMError();
            err = ENOMEM;
            goto error;
        }
        while ((rc = getpwuid_r(uid, &pwd, buf, bufsize,
                                &pwd_result)) == ERANGE) {
            if (VIR_RESIZE_N(buf, bufsize, bufsize, bufsize) < 0) {
                virReportOOMError();
                err = ENOMEM;
                goto error;
            }
        }

        if (rc) {
            virReportSystemError(err = rc, _("cannot getpwuid_r(%u)"),
                                 (unsigned int) uid);
            goto error;
        }

        if (!pwd_result) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("getpwuid_r failed to retrieve data "
                             "for uid '%u'"),
                           (unsigned int) uid);
            err = EINVAL;
            goto error;
        }

        if (initgroups(pwd.pw_name, pwd.pw_gid) < 0) {
            virReportSystemError(err = errno,
                                 _("cannot initgroups(\"%s\", %d)"),
                                 pwd.pw_name, (unsigned int) pwd.pw_gid);
            goto error;
        }
# endif
        if (setreuid(uid, uid) < 0) {
            virReportSystemError(err = errno,
                                 _("cannot change to uid to '%u'"),
                                 (unsigned int) uid);
            goto error;
        }
    }

    VIR_FREE(buf);
    return 0;

error:
    VIR_FREE(buf);
    errno = err;
    return -1;
}

#else /* ! HAVE_GETPWUID_R */

# ifdef WIN32
/* These methods are adapted from GLib2 under terms of LGPLv2+ */
static int
virGetWin32SpecialFolder(int csidl, char **path)
{
    char buf[MAX_PATH+1];
    LPITEMIDLIST pidl = NULL;
    int ret = 0;

    *path = NULL;

    if (SHGetSpecialFolderLocation(NULL, csidl, &pidl) == S_OK) {
        if (SHGetPathFromIDList(pidl, buf)) {
            if (!(*path = strdup(buf))) {
                virReportOOMError();
                ret = -1;
            }
        }
        CoTaskMemFree(pidl);
    }
    return ret;
}

static int
virGetWin32DirectoryRoot(char **path)
{
    char windowsdir[MAX_PATH];
    int ret = 0;

    *path = NULL;

    if (GetWindowsDirectory(windowsdir, ARRAY_CARDINALITY(windowsdir)))
    {
        const char *tmp;
        /* Usually X:\Windows, but in terminal server environments
         * might be an UNC path, AFAIK.
         */
        tmp = virFileSkipRoot(windowsdir);
        if (VIR_FILE_IS_DIR_SEPARATOR(tmp[-1]) &&
            tmp[-2] != ':')
            tmp--;

        windowsdir[tmp - windowsdir] = '\0';
    } else {
        strcpy(windowsdir, "C:\\");
    }

    if (!(*path = strdup(windowsdir))) {
        virReportOOMError();
        ret = -1;
    }

    return ret;
}



char *
virGetUserDirectory(void)
{
    const char *dir;
    char *ret;

    dir = getenv("HOME");

    /* Only believe HOME if it is an absolute path and exists */
    if (dir) {
        if (!virFileIsAbsPath(dir) ||
            !virFileExists(dir))
            dir = NULL;
    }

    /* In case HOME is Unix-style (it happens), convert it to
     * Windows style.
     */
    if (dir) {
        char *p;
        while ((p = strchr(dir, '/')) != NULL)
            *p = '\\';
    }

    if (!dir)
        /* USERPROFILE is probably the closest equivalent to $HOME? */
        dir = getenv("USERPROFILE");

    if (dir) {
        if (!(ret = strdup(dir))) {
            virReportOOMError();
            return NULL;
        }
    }

    if (!ret &&
        virGetWin32SpecialFolder(CSIDL_PROFILE, &ret) < 0)
        return NULL;

    if (!ret &&
        virGetWin32DirectoryRoot(&ret) < 0)
        return NULL;

    if (!ret) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to determine home directory"));
        return NULL;
    }

    return ret;
}

char *
virGetUserConfigDirectory(void)
{
    char *ret;
    if (virGetWin32SpecialFolder(CSIDL_LOCAL_APPDATA, &ret) < 0)
        return NULL;

    if (!ret) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to determine config directory"));
        return NULL;
    }
    return ret;
}

char *
virGetUserCacheDirectory(void)
{
    char *ret;
    if (virGetWin32SpecialFolder(CSIDL_INTERNET_CACHE, &ret) < 0)
        return NULL;

    if (!ret) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to determine config directory"));
        return NULL;
    }
    return ret;
}

char *
virGetUserRuntimeDirectory(void)
{
    return virGetUserCacheDirectory();
}
# else /* !HAVE_GETPWUID_R && !WIN32 */
char *
virGetUserDirectory(void)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetUserDirectory is not available"));

    return NULL;
}

char *
virGetUserConfigDirectory(void)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetUserConfigDirectory is not available"));

    return NULL;
}

char *
virGetUserCacheDirectory(void)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetUserCacheDirectory is not available"));

    return NULL;
}

char *
virGetUserRuntimeDirectory(void)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetUserRuntimeDirectory is not available"));

    return NULL;
}
# endif /* ! HAVE_GETPWUID_R && ! WIN32 */

char *
virGetUserName(uid_t uid ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetUserName is not available"));

    return NULL;
}

int virGetUserID(const char *name ATTRIBUTE_UNUSED,
                 uid_t *uid ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetUserID is not available"));

    return 0;
}


int virGetGroupID(const char *name ATTRIBUTE_UNUSED,
                  gid_t *gid ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetGroupID is not available"));

    return 0;
}

int
virSetUIDGID(uid_t uid ATTRIBUTE_UNUSED,
             gid_t gid ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virSetUIDGID is not available"));
    return -1;
}

char *
virGetGroupName(gid_t gid ATTRIBUTE_UNUSED)
{
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   "%s", _("virGetGroupName is not available"));

    return NULL;
}
#endif /* HAVE_GETPWUID_R */

#if WITH_CAPNG
/* Set the real and effective uid and gid to the given values, while
 * maintaining the capabilities indicated by bits in @capBits. Return
 * 0 on success, -1 on failure (the original system error remains in
 * errno).
 */
int
virSetUIDGIDWithCaps(uid_t uid, gid_t gid, unsigned long long capBits,
                     bool clearExistingCaps)
{
    int ii, capng_ret, ret = -1;
    bool need_setgid = false, need_setuid = false;
    bool need_setpcap = false;

    /* First drop all caps (unless the requested uid is "unchanged" or
     * root and clearExistingCaps wasn't requested), then add back
     * those in capBits + the extra ones we need to change uid/gid and
     * change the capabilities bounding set.
     */

    if (clearExistingCaps || (uid != (uid_t)-1 && uid != 0))
       capng_clear(CAPNG_SELECT_BOTH);

    for (ii = 0; ii <= CAP_LAST_CAP; ii++) {
        if (capBits & (1ULL << ii)) {
            capng_update(CAPNG_ADD,
                         CAPNG_EFFECTIVE|CAPNG_INHERITABLE|
                         CAPNG_PERMITTED|CAPNG_BOUNDING_SET,
                         ii);
        }
    }

    if (gid != (gid_t)-1 &&
        !capng_have_capability(CAPNG_EFFECTIVE, CAP_SETGID)) {
        need_setgid = true;
        capng_update(CAPNG_ADD, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_SETGID);
    }
    if (uid != (uid_t)-1 &&
        !capng_have_capability(CAPNG_EFFECTIVE, CAP_SETUID)) {
        need_setuid = true;
        capng_update(CAPNG_ADD, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_SETUID);
    }
# ifdef PR_CAPBSET_DROP
    /* If newer kernel, we need also need setpcap to change the bounding set */
    if ((capBits || need_setgid || need_setuid) &&
        !capng_have_capability(CAPNG_EFFECTIVE, CAP_SETPCAP)) {
        need_setpcap = true;
    }
    if (need_setpcap)
        capng_update(CAPNG_ADD, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_SETPCAP);
# endif

    /* Tell system we want to keep caps across uid change */
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0)) {
        virReportSystemError(errno, "%s",
                             _("prctl failed to set KEEPCAPS"));
        goto cleanup;
    }

    /* Change to the temp capabilities */
    if ((capng_ret = capng_apply(CAPNG_SELECT_CAPS)) < 0) {
        /* Failed.  If we are running unprivileged, and the arguments make sense
         * for this scenario, assume we're starting some kind of setuid helper:
         * do not set any of capBits in the permitted or effective sets, and let
         * the program get them on its own.
         *
         * (Too bad we cannot restrict the bounding set to the capabilities we
         * would like the helper to have!).
         */
        if (getuid() > 0 && clearExistingCaps && !need_setuid && !need_setgid) {
            capng_clear(CAPNG_SELECT_CAPS);
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot apply process capabilities %d"), capng_ret);
            goto cleanup;
        }
    }

    if (virSetUIDGID(uid, gid) < 0)
        goto cleanup;

    /* Tell it we are done keeping capabilities */
    if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0)) {
        virReportSystemError(errno, "%s",
                             _("prctl failed to reset KEEPCAPS"));
        goto cleanup;
    }

    /* Set bounding set while we have CAP_SETPCAP.  Unfortunately we cannot
     * do this if we failed to get the capability above, so ignore the
     * return value.
     */
    capng_apply(CAPNG_SELECT_BOUNDS);

    /* Drop the caps that allow setuid/gid (unless they were requested) */
    if (need_setgid)
        capng_update(CAPNG_DROP, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_SETGID);
    if (need_setuid)
        capng_update(CAPNG_DROP, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_SETUID);
    /* Throw away CAP_SETPCAP so no more changes */
    if (need_setpcap)
        capng_update(CAPNG_DROP, CAPNG_EFFECTIVE|CAPNG_PERMITTED, CAP_SETPCAP);

    if (((capng_ret = capng_apply(CAPNG_SELECT_CAPS)) < 0)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot apply process capabilities %d"), capng_ret);
        ret = -1;
        goto cleanup;
    }

    ret = 0;
cleanup:
    return ret;
}

#else
/*
 * On platforms without libcapng, the capabilities setting is treated
 * as a NOP.
 */

int
virSetUIDGIDWithCaps(uid_t uid, gid_t gid,
                     unsigned long long capBits ATTRIBUTE_UNUSED,
                     bool clearExistingCaps ATTRIBUTE_UNUSED)
{
    return virSetUIDGID(uid, gid);
}
#endif


#if defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R
/* search /proc/mounts for mount point of *type; return pointer to
 * malloc'ed string of the path if found, otherwise return NULL
 * with errno set to an appropriate value.
 */
char *virFileFindMountPoint(const char *type)
{
    FILE *f;
    struct mntent mb;
    char mntbuf[1024];
    char *ret = NULL;

    f = setmntent("/proc/mounts", "r");
    if (!f)
        return NULL;

    while (getmntent_r(f, &mb, mntbuf, sizeof(mntbuf))) {
        if (STREQ(mb.mnt_type, type)) {
            ret = strdup(mb.mnt_dir);
            goto cleanup;
        }
    }

    if (!ret)
        errno = ENOENT;

cleanup:
    endmntent(f);

    return ret;
}

#else /* defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R */

char *
virFileFindMountPoint(const char *type ATTRIBUTE_UNUSED)
{
    errno = ENOSYS;

    return NULL;
}

#endif /* defined HAVE_MNTENT_H && defined HAVE_GETMNTENT_R */

#if defined(UDEVADM) || defined(UDEVSETTLE)
void virFileWaitForDevices(void)
{
# ifdef UDEVADM
    const char *const settleprog[] = { UDEVADM, "settle", NULL };
# else
    const char *const settleprog[] = { UDEVSETTLE, NULL };
# endif
    int exitstatus;

    if (access(settleprog[0], X_OK) != 0)
        return;

    /*
     * NOTE: we ignore errors here; this is just to make sure that any device
     * nodes that are being created finish before we try to scan them.
     * If this fails for any reason, we still have the backup of polling for
     * 5 seconds for device nodes.
     */
    if (virRun(settleprog, &exitstatus) < 0)
    {}
}
#else
void virFileWaitForDevices(void) {}
#endif

int virBuildPathInternal(char **path, ...)
{
    char *path_component = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    va_list ap;
    int ret = 0;

    va_start(ap, path);

    path_component = va_arg(ap, char *);
    virBufferAdd(&buf, path_component, -1);

    while ((path_component = va_arg(ap, char *)) != NULL)
    {
        virBufferAddChar(&buf, '/');
        virBufferAdd(&buf, path_component, -1);
    }

    va_end(ap);

    *path = virBufferContentAndReset(&buf);
    if (*path == NULL) {
        ret = -1;
    }

    return ret;
}

#if HAVE_LIBDEVMAPPER_H
bool
virIsDevMapperDevice(const char *dev_name)
{
    struct stat buf;

    if (!stat(dev_name, &buf) &&
        S_ISBLK(buf.st_mode) &&
        dm_is_dm_major(major(buf.st_rdev)))
            return true;

    return false;
}
#else
bool virIsDevMapperDevice(const char *dev_name ATTRIBUTE_UNUSED)
{
    return false;
}
#endif

bool
virValidateWWN(const char *wwn) {
    int i;
    const char *p = wwn;

    if (STRPREFIX(wwn, "0x")) {
        p += 2;
    }

    for (i = 0; p[i]; i++) {
        if (!c_isxdigit(p[i]))
            break;
    }

    if (i != 16 || p[i]) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Malformed wwn: %s"));
        return false;
    }

    return true;
}

bool
virStrIsPrint(const char *str)
{
    int i;

    for (i = 0; str[i]; i++)
        if (!c_isprint(str[i]))
            return false;

    return true;
}

#if defined(major) && defined(minor)
int
virGetDeviceID(const char *path, int *maj, int *min)
{
    struct stat sb;

    if (stat(path, &sb) < 0)
        return -errno;

    if (!S_ISBLK(sb.st_mode))
        return -EINVAL;

    if (maj)
        *maj = major(sb.st_rdev);
    if (min)
        *min = minor(sb.st_rdev);

    return 0;
}
#else
int
virGetDeviceID(const char *path ATTRIBUTE_UNUSED,
               int *maj ATTRIBUTE_UNUSED,
               int *min ATTRIBUTE_UNUSED)
{

    return -ENOSYS;
}
#endif

#define SYSFS_DEV_BLOCK_PATH "/sys/dev/block"

char *
virGetUnprivSGIOSysfsPath(const char *path,
                          const char *sysfs_dir)
{
    int maj, min;
    char *sysfs_path = NULL;
    int rc;

    if ((rc = virGetDeviceID(path, &maj, &min)) < 0) {
        virReportSystemError(-rc,
                             _("Unable to get device ID '%s'"),
                             path);
        return NULL;
    }

    if (virAsprintf(&sysfs_path, "%s/%d:%d/queue/unpriv_sgio",
                    sysfs_dir ? sysfs_dir : SYSFS_DEV_BLOCK_PATH,
                    maj, min) < 0) {
        virReportOOMError();
        return NULL;
    }

    return sysfs_path;
}

int
virSetDeviceUnprivSGIO(const char *path,
                       const char *sysfs_dir,
                       int unpriv_sgio)
{
    char *sysfs_path = NULL;
    char *val = NULL;
    int ret = -1;
    int rc;

    if (!(sysfs_path = virGetUnprivSGIOSysfsPath(path, sysfs_dir)))
        return -1;

    if (!virFileExists(sysfs_path)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("unpriv_sgio is not supported by this kernel"));
        goto cleanup;
    }

    if (virAsprintf(&val, "%d", unpriv_sgio) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if ((rc = virFileWriteStr(sysfs_path, val, 0)) < 0) {
        virReportSystemError(-rc, _("failed to set %s"), sysfs_path);
        goto cleanup;
    }

    ret = 0;
cleanup:
    VIR_FREE(sysfs_path);
    VIR_FREE(val);
    return ret;
}

int
virGetDeviceUnprivSGIO(const char *path,
                       const char *sysfs_dir,
                       int *unpriv_sgio)
{
    char *sysfs_path = NULL;
    char *buf = NULL;
    char *tmp = NULL;
    int ret = -1;

    if (!(sysfs_path = virGetUnprivSGIOSysfsPath(path, sysfs_dir)))
        return -1;

    if (!virFileExists(sysfs_path)) {
        virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("unpriv_sgio is not supported by this kernel"));
        goto cleanup;
    }

    if (virFileReadAll(sysfs_path, 1024, &buf) < 0)
        goto cleanup;

    if ((tmp = strchr(buf, '\n')))
        *tmp = '\0';

    if (virStrToLong_i(buf, NULL, 10, unpriv_sgio) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("failed to parse value of %s"), sysfs_path);
        goto cleanup;
    }

    ret = 0;
cleanup:
    VIR_FREE(sysfs_path);
    VIR_FREE(buf);
    return ret;
}

#ifdef __linux__
# define SYSFS_FC_HOST_PATH "/sys/class/fc_host/"
# define SYSFS_SCSI_HOST_PATH "/sys/class/scsi_host/"

/* virReadFCHost:
 * @sysfs_prefix: "fc_host" sysfs path, defaults to SYSFS_FC_HOST_PATH
 * @host: Host number, E.g. 5 of "fc_host/host5"
 * @entry: Name of the sysfs entry to read
 * @result: Return the entry value as string
 *
 * Read the value of sysfs "fc_host" entry.
 *
 * Returns 0 on success, and @result is filled with the entry value.
 * as string, Otherwise returns -1. Caller must free @result after
 * use.
 */
int
virReadFCHost(const char *sysfs_prefix,
              int host,
              const char *entry,
              char **result)
{
    char *sysfs_path = NULL;
    char *p = NULL;
    int ret = -1;
    char *buf = NULL;

    if (virAsprintf(&sysfs_path, "%s/host%d/%s",
                    sysfs_prefix ? sysfs_prefix : SYSFS_FC_HOST_PATH,
                    host, entry) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (virFileReadAll(sysfs_path, 1024, &buf) < 0)
        goto cleanup;

    if ((p = strchr(buf, '\n')))
        *p = '\0';

    if ((p = strstr(buf, "0x")))
        p += strlen("0x");
    else
        p = buf;

    if (!(*result = strndup(p, sizeof(buf)))) {
        virReportOOMError();
        goto cleanup;
    }

    ret = 0;
cleanup:
    VIR_FREE(sysfs_path);
    VIR_FREE(buf);
    return ret;
}

int
virIsCapableFCHost(const char *sysfs_prefix,
                   int host)
{
    char *sysfs_path = NULL;
    int ret = -1;

    if (virAsprintf(&sysfs_path, "%shost%d",
                    sysfs_prefix ? sysfs_prefix : SYSFS_FC_HOST_PATH,
                    host) < 0) {
        virReportOOMError();
        return -1;
    }

    if (access(sysfs_path, F_OK) == 0)
        ret = 0;

    VIR_FREE(sysfs_path);
    return ret;
}

int
virIsCapableVport(const char *sysfs_prefix,
                  int host)
{
    char *scsi_host_path = NULL;
    char *fc_host_path = NULL;
    int ret = -1;

    if (virAsprintf(&fc_host_path,
                    "%shost%d%s",
                    sysfs_prefix ? sysfs_prefix : SYSFS_FC_HOST_PATH,
                    host,
                    "vport_create") < 0) {
        virReportOOMError();
        return -1;
    }

    if (virAsprintf(&scsi_host_path,
                    "%shost%d%s",
                    sysfs_prefix ? sysfs_prefix : SYSFS_SCSI_HOST_PATH,
                    host,
                    "vport_create") < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if ((access(fc_host_path, F_OK) == 0) ||
        (access(scsi_host_path, F_OK) == 0))
        ret = 0;

cleanup:
    VIR_FREE(fc_host_path);
    VIR_FREE(scsi_host_path);
    return ret;
}

int
virManageVport(const int parent_host,
               const char *wwpn,
               const char *wwnn,
               int operation)
{
    int ret = -1;
    char *operation_path = NULL, *vport_name = NULL;
    const char *operation_file = NULL;

    switch (operation) {
    case VPORT_CREATE:
        operation_file = "vport_create";
        break;
    case VPORT_DELETE:
        operation_file = "vport_delete";
        break;
    default:
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("Invalid vport operation (%d)"), operation);
        goto cleanup;
    }

    if (virAsprintf(&operation_path,
                    "%shost%d/%s",
                    SYSFS_FC_HOST_PATH,
                    parent_host,
                    operation_file) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (!virFileExists(operation_path)) {
        VIR_FREE(operation_path);
        if (virAsprintf(&operation_path,
                        "%shost%d/%s",
                        SYSFS_SCSI_HOST_PATH,
                        parent_host,
                        operation_file) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (!virFileExists(operation_path)) {
            virReportError(VIR_ERR_OPERATION_INVALID,
                           _("vport operation '%s' is not supported for host%d"),
                           operation_file, parent_host);
            goto cleanup;
        }
    }

    if (virAsprintf(&vport_name,
                    "%s:%s",
                    wwnn,
                    wwpn) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (virFileWriteStr(operation_path, vport_name, 0) == 0)
        ret = 0;
    else
        virReportSystemError(errno,
                             _("Write of '%s' to '%s' during "
                               "vport create/delete failed"),
                             vport_name, operation_path);

cleanup:
    VIR_FREE(vport_name);
    VIR_FREE(operation_path);
    return ret;
}

/* virGetHostNameByWWN:
 *
 * Iterate over the sysfs tree to get SCSI host name (e.g. scsi_host5)
 * by wwnn,wwpn pair.
 */
char *
virGetFCHostNameByWWN(const char *sysfs_prefix,
                      const char *wwnn,
                      const char *wwpn)
{
    const char *prefix = sysfs_prefix ? sysfs_prefix : SYSFS_FC_HOST_PATH;
    struct dirent *entry = NULL;
    DIR *dir = NULL;
    char *wwnn_path = NULL;
    char *wwpn_path = NULL;
    char *wwnn_buf = NULL;
    char *wwpn_buf = NULL;
    char *p;
    char *ret = NULL;

    if (!(dir = opendir(prefix))) {
        virReportSystemError(errno,
                             _("Failed to opendir path '%s'"),
                             prefix);
        return NULL;
    }

# define READ_WWN(wwn_path, buf)                      \
    do {                                              \
        if (virFileReadAll(wwn_path, 1024, &buf) < 0) \
            goto cleanup;                             \
        if ((p = strchr(buf, '\n')))                  \
            *p = '\0';                                \
        if (STRPREFIX(buf, "0x"))                     \
            p = buf + strlen("0x");                   \
        else                                          \
            p = buf;                                  \
    } while (0)

    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;

        if (virAsprintf(&wwnn_path, "%s%s/node_name", prefix,
                        entry->d_name) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (!virFileExists(wwnn_path)) {
            VIR_FREE(wwnn_path);
            continue;
        }

        READ_WWN(wwnn_path, wwnn_buf);

        if (STRNEQ(wwnn, p)) {
            VIR_FREE(wwnn_buf);
            VIR_FREE(wwnn_path);
            continue;
        }

        if (virAsprintf(&wwpn_path, "%s%s/port_name", prefix,
                        entry->d_name) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (!virFileExists(wwpn_path)) {
            VIR_FREE(wwnn_buf);
            VIR_FREE(wwnn_path);
            VIR_FREE(wwpn_path);
            continue;
        }

        READ_WWN(wwpn_path, wwpn_buf);

        if (STRNEQ(wwpn, p)) {
            VIR_FREE(wwnn_path);
            VIR_FREE(wwpn_path);
            VIR_FREE(wwnn_buf);
            VIR_FREE(wwpn_buf);
            continue;
        }

        ret = strdup(entry->d_name);
        break;
    }

cleanup:
# undef READ_WWN
    closedir(dir);
    VIR_FREE(wwnn_path);
    VIR_FREE(wwpn_path);
    VIR_FREE(wwnn_buf);
    VIR_FREE(wwpn_buf);
    return ret;
}

# define PORT_STATE_ONLINE "Online"

/* virFindFCHostCapableVport:
 *
 * Iterate over the sysfs and find out the first online HBA which
 * supports vport, and not saturated,.
 */
char *
virFindFCHostCapableVport(const char *sysfs_prefix)
{
    const char *prefix = sysfs_prefix ? sysfs_prefix : SYSFS_FC_HOST_PATH;
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    char *max_vports = NULL;
    char *vports = NULL;
    char *state = NULL;
    char *ret = NULL;

    if (!(dir = opendir(prefix))) {
        virReportSystemError(errno,
                             _("Failed to opendir path '%s'"),
                             prefix);
        return NULL;
    }

    while ((entry = readdir(dir))) {
        unsigned int host;
        char *p = NULL;

        if (entry->d_name[0] == '.')
            continue;

        p = entry->d_name + strlen("host");
        if (virStrToLong_ui(p, NULL, 10, &host) == -1) {
            VIR_DEBUG("Failed to parse host number from '%s'",
                      entry->d_name);
            continue;
        }

        if (!virIsCapableVport(NULL, host))
            continue;

        if (virReadFCHost(NULL, host, "port_state", &state) < 0) {
             VIR_DEBUG("Failed to read port_state for host%d", host);
             continue;
        }

        /* Skip the not online FC host */
        if (STRNEQ(state, PORT_STATE_ONLINE)) {
            VIR_FREE(state);
            continue;
        }
        VIR_FREE(state);

        if (virReadFCHost(NULL, host, "max_npiv_vports", &max_vports) < 0) {
             VIR_DEBUG("Failed to read max_npiv_vports for host%d", host);
             continue;
        }

        if (virReadFCHost(NULL, host, "npiv_vports_inuse", &vports) < 0) {
             VIR_DEBUG("Failed to read npiv_vports_inuse for host%d", host);
             VIR_FREE(max_vports);
             continue;
        }

        /* Compare from the strings directly, instead of converting
         * the strings to integers first
         */
        if ((strlen(max_vports) >= strlen(vports)) ||
            ((strlen(max_vports) == strlen(vports)) &&
             strcmp(max_vports, vports) > 0)) {
            ret = strdup(entry->d_name);
            goto cleanup;
        }

        VIR_FREE(max_vports);
        VIR_FREE(vports);
    }

cleanup:
    closedir(dir);
    VIR_FREE(max_vports);
    VIR_FREE(vports);
    return ret;
}
#else
int
virReadFCHost(const char *sysfs_prefix ATTRIBUTE_UNUSED,
              int host ATTRIBUTE_UNUSED,
              const char *entry ATTRIBUTE_UNUSED,
              char **result ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s", _("Not supported on this platform"));
    return -1;
}

int
virIsCapableFCHost(const char *sysfs_prefix ATTRIBUTE_UNUSED,
                   int host ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s", _("Not supported on this platform"));
    return -1;
}

int
virIsCapableVport(const char *sysfs_prefix ATTRIBUTE_UNUSED,
                  int host ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s", _("Not supported on this platform"));
    return -1;
}

int
virManageVport(const int parent_host ATTRIBUTE_UNUSED,
               const char *wwpn ATTRIBUTE_UNUSED,
               const char *wwnn ATTRIBUTE_UNUSED,
               int operation ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s", _("Not supported on this platform"));
    return -1;
}

char *
virGetFCHostNameByWWN(const char *sysfs_prefix ATTRIBUTE_UNUSED,
                      const char *wwnn ATTRIBUTE_UNUSED,
                      const char *wwpn ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s", _("Not supported on this platform"));
    return NULL;
}

char *
virFindFCHostCapableVport(const char *sysfs_prefix ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s", _("Not supported on this platform"));
    return NULL;
}

#endif /* __linux__ */

/**
 * virCompareLimitUlong:
 *
 * Compare two unsigned long long numbers. Value '0' of the arguments has a
 * special meaning of 'unlimited' and thus greater than any other value.
 *
 * Returns 0 if the numbers are equal, -1 if b is greater, 1 if a is greater.
 */
int
virCompareLimitUlong(unsigned long long a, unsigned long b)
{
    if (a == b)
        return 0;

    if (a == 0 || a > b)
        return 1;

    return -1;
}
