/*
 * virutil.h: common, generic utility functions
 *
 * Copyright (C) 2010-2013 Red Hat, Inc.
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
 * File created Jul 18, 2007 - Shuveb Hussain <shuveb@binarykarma.com>
 */

#ifndef __VIR_UTIL_H__
# define __VIR_UTIL_H__

# include "verify.h"
# include "internal.h"
# include <unistd.h>
# include <sys/select.h>
# include <sys/types.h>

# ifndef MIN
#  define MIN(a, b) ((a) < (b) ? (a) : (b))
# endif
# ifndef MAX
#  define MAX(a, b) ((a) > (b) ? (a) : (b))
# endif

ssize_t saferead(int fd, void *buf, size_t count) ATTRIBUTE_RETURN_CHECK;
ssize_t safewrite(int fd, const void *buf, size_t count)
    ATTRIBUTE_RETURN_CHECK;
int safezero(int fd, off_t offset, off_t len)
    ATTRIBUTE_RETURN_CHECK;

int virSetBlocking(int fd, bool blocking) ATTRIBUTE_RETURN_CHECK;
int virSetNonBlock(int fd) ATTRIBUTE_RETURN_CHECK;
int virSetInherit(int fd, bool inherit) ATTRIBUTE_RETURN_CHECK;
int virSetCloseExec(int fd) ATTRIBUTE_RETURN_CHECK;

int virPipeReadUntilEOF(int outfd, int errfd,
                        char **outbuf, char **errbuf);

int virSetUIDGID(uid_t uid, gid_t gid);
int virSetUIDGIDWithCaps(uid_t uid, gid_t gid, unsigned long long capBits,
                         bool clearExistingCaps);

int virFileReadLimFD(int fd, int maxlen, char **buf) ATTRIBUTE_RETURN_CHECK;

int virFileReadAll(const char *path, int maxlen, char **buf) ATTRIBUTE_RETURN_CHECK;

int virFileWriteStr(const char *path, const char *str, mode_t mode)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_RETURN_CHECK;

int virFileMatchesNameSuffix(const char *file,
                             const char *name,
                             const char *suffix);

int virFileHasSuffix(const char *str,
                     const char *suffix);

int virFileStripSuffix(char *str,
                       const char *suffix) ATTRIBUTE_RETURN_CHECK;

int virFileLinkPointsTo(const char *checkLink,
                        const char *checkDest);

int virFileResolveLink(const char *linkpath,
                       char **resultpath) ATTRIBUTE_RETURN_CHECK;
int virFileResolveAllLinks(const char *linkpath,
                           char **resultpath) ATTRIBUTE_RETURN_CHECK;

int virFileIsLink(const char *linkpath)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_RETURN_CHECK;

char *virFindFileInPath(const char *file);

bool virFileIsDir (const char *file) ATTRIBUTE_NONNULL(1);
bool virFileExists(const char *file) ATTRIBUTE_NONNULL(1);
bool virFileIsExecutable(const char *file) ATTRIBUTE_NONNULL(1);

char *virFileSanitizePath(const char *path);

enum {
    VIR_FILE_OPEN_NONE        = 0,
    VIR_FILE_OPEN_NOFORK      = (1 << 0),
    VIR_FILE_OPEN_FORK        = (1 << 1),
    VIR_FILE_OPEN_FORCE_MODE  = (1 << 2),
    VIR_FILE_OPEN_FORCE_OWNER = (1 << 3),
};
int virFileAccessibleAs(const char *path, int mode,
                        uid_t uid, gid_t gid)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_RETURN_CHECK;
int virFileOpenAs(const char *path, int openflags, mode_t mode,
                  uid_t uid, gid_t gid,
                  unsigned int flags)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_RETURN_CHECK;

enum {
    VIR_DIR_CREATE_NONE        = 0,
    VIR_DIR_CREATE_AS_UID      = (1 << 0),
    VIR_DIR_CREATE_FORCE_PERMS = (1 << 1),
    VIR_DIR_CREATE_ALLOW_EXIST = (1 << 2),
};
int virDirCreate(const char *path, mode_t mode, uid_t uid, gid_t gid,
                 unsigned int flags) ATTRIBUTE_RETURN_CHECK;
int virFileMakePath(const char *path) ATTRIBUTE_RETURN_CHECK;
int virFileMakePathWithMode(const char *path,
                            mode_t mode) ATTRIBUTE_RETURN_CHECK;

char *virFileBuildPath(const char *dir,
                       const char *name,
                       const char *ext) ATTRIBUTE_RETURN_CHECK;


# ifdef WIN32
/* On Win32, the canonical directory separator is the backslash, and
 * the search path separator is the semicolon. Note that also the
 * (forward) slash works as directory separator.
 */
#  define VIR_FILE_DIR_SEPARATOR '\\'
#  define VIR_FILE_DIR_SEPARATOR_S "\\"
#  define VIR_FILE_IS_DIR_SEPARATOR(c) ((c) == VIR_FILE_DIR_SEPARATOR || (c) == '/')
#  define VIR_FILE_PATH_SEPARATOR ';'
#  define VIR_FILE_PATH_SEPARATOR_S ";"

# else  /* !WIN32 */

#  define VIR_FILE_DIR_SEPARATOR '/'
#  define VIR_FILE_DIR_SEPARATOR_S "/"
#  define VIR_FILE_IS_DIR_SEPARATOR(c) ((c) == VIR_FILE_DIR_SEPARATOR)
#  define VIR_FILE_PATH_SEPARATOR ':'
#  define VIR_FILE_PATH_SEPARATOR_S ":"

# endif /* !WIN32 */

bool virFileIsAbsPath(const char *path);
int virFileAbsPath(const char *path,
                   char **abspath) ATTRIBUTE_RETURN_CHECK;
const char *virFileSkipRoot(const char *path);

int virFileOpenTty(int *ttymaster,
                   char **ttyName,
                   int rawmode);

int virScaleInteger(unsigned long long *value, const char *suffix,
                    unsigned long long scale, unsigned long long limit)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_RETURN_CHECK;

int virHexToBin(unsigned char c);

int virParseNumber(const char **str);
int virParseVersionString(const char *str, unsigned long *version,
                          bool allowMissing);

int virDoubleToStr(char **strp, double number)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_RETURN_CHECK;

char *virFormatIntDecimal(char *buf, size_t buflen, int val)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_RETURN_CHECK;

int virDiskNameToIndex(const char* str);
char *virIndexToDiskName(int idx, const char *prefix);

int virEnumFromString(const char *const*types,
                      unsigned int ntypes,
                      const char *type);

const char *virEnumToString(const char *const*types,
                            unsigned int ntypes,
                            int type);

# define VIR_ENUM_IMPL(name, lastVal, ...)                               \
    static const char *const name ## TypeList[] = { __VA_ARGS__ };      \
    verify(ARRAY_CARDINALITY(name ## TypeList) == lastVal);             \
    const char *name ## TypeToString(int type) {                        \
        return virEnumToString(name ## TypeList,                        \
                               ARRAY_CARDINALITY(name ## TypeList),     \
                               type);                                   \
    }                                                                   \
    int name ## TypeFromString(const char *type) {                      \
        return virEnumFromString(name ## TypeList,                      \
                                 ARRAY_CARDINALITY(name ## TypeList),   \
                                 type);                                 \
    }

# define VIR_ENUM_DECL(name)                             \
    const char *name ## TypeToString(int type);         \
    int name ## TypeFromString(const char*type);

# ifndef HAVE_GETUID
static inline int getuid (void) { return 0; }
# endif

# ifndef HAVE_GETEUID
static inline int geteuid (void) { return 0; }
# endif

# ifndef HAVE_GETGID
static inline int getgid (void) { return 0; }
# endif

char *virGetHostname(virConnectPtr conn);

char *virGetUserDirectory(void);
char *virGetUserConfigDirectory(void);
char *virGetUserCacheDirectory(void);
char *virGetUserRuntimeDirectory(void);
char *virGetUserName(uid_t uid);
char *virGetGroupName(gid_t gid);
int virGetUserID(const char *name,
                 uid_t *uid) ATTRIBUTE_RETURN_CHECK;
int virGetGroupID(const char *name,
                  gid_t *gid) ATTRIBUTE_RETURN_CHECK;

char *virFileFindMountPoint(const char *type);

void virFileWaitForDevices(void);

# define virBuildPath(path, ...) virBuildPathInternal(path, __VA_ARGS__, NULL)
int virBuildPathInternal(char **path, ...) ATTRIBUTE_SENTINEL;

bool virIsDevMapperDevice(const char *dev_name) ATTRIBUTE_NONNULL(1);

bool virValidateWWN(const char *wwn);

bool virStrIsPrint(const char *str);

int virGetDeviceID(const char *path,
                   int *maj,
                   int *min);
int virSetDeviceUnprivSGIO(const char *path,
                           const char *sysfs_dir,
                           int unpriv_sgio);
int virGetDeviceUnprivSGIO(const char *path,
                           const char *sysfs_dir,
                           int *unpriv_sgio);
char *virGetUnprivSGIOSysfsPath(const char *path,
                                const char *sysfs_dir);
int virReadFCHost(const char *sysfs_prefix,
                  int host,
                  const char *entry,
                  char **result)
    ATTRIBUTE_NONNULL(3) ATTRIBUTE_NONNULL(4);

int virIsCapableFCHost(const char *sysfs_prefix, int host);
int virIsCapableVport(const char *sysfs_prefix, int host);

enum {
    VPORT_CREATE,
    VPORT_DELETE,
};

int virManageVport(const int parent_host,
                   const char *wwpn,
                   const char *wwnn,
                   int operation)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);

char *virGetFCHostNameByWWN(const char *sysfs_prefix,
                            const char *wwnn,
                            const char *wwpn)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3);

char *virFindFCHostCapableVport(const char *sysfs_prefix);

int virCompareLimitUlong(unsigned long long a, unsigned long b);

#endif /* __VIR_UTIL_H__ */
