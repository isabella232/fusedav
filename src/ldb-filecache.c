/***
  This file is part of fusedav.

  fusedav is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  fusedav is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU General Public License
  along with fusedav; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "ldb-filecache.h"
#include "statcache.h"
#include "fusedav.h"
#include "log.h"

#include <ne_uri.h>

#define REFRESH_INTERVAL 3

typedef int fd_t;

// Session data
struct ldb_filecache_sdata {
    fd_t fd; // LOCK_SH for write/truncation; LOCK_EX during PUT
    char filename[PATH_MAX]; // Only used for new replacement files.
    bool readable;
    bool writable;
    bool modified;
};

// FIX ME Where to find ETAG_MAX?
#define ETAG_MAX 256

// Persistent data stored in leveldb
struct ldb_filecache_pdata {
    char filename[PATH_MAX];
    char etag[ETAG_MAX + 1];
    time_t last_server_update;
};

int ldb_filecache_init(char *cache_path) {
    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "%s/files", cache_path);
    if (mkdir(cache_path, 0770) == -1) {
        if (errno != EEXIST) {
            log_print(LOG_ERR, "Cache Path %s could not be created.", cache_path);
            return -1;
        }
    }
    if (mkdir(path, 0770) == -1) {
        if (errno != EEXIST) {
            log_print(LOG_ERR, "Path %s could not be created.", path);
            return -1;
        }
    }
    return 0;
}

// Allocates a new string.
static char *path2key(const char *path) {
    char *key = NULL;
    asprintf(&key, "fc:%s", path);
    return key;
}

// creates a new cache file
static int new_cache_file(const char *cache_path, char *cache_file_path, fd_t *fd) {
    snprintf(cache_file_path, PATH_MAX, "%s/files/fusedav-cache-XXXXXX", cache_path);
    log_print(LOG_DEBUG, "Using pattern %s", cache_file_path);
    if ((*fd = mkstemp(cache_file_path)) < 0) {
        log_print(LOG_ERR, "new_cache_file: Failed mkstemp: errno = %d %s", errno, strerror(errno));
        return -1;
    }

    log_print(LOG_DEBUG, "new_cache_file: mkstemp fd=%d :: %s", *fd, cache_file_path);
    return 0;
}

// adds an entry to the ldb cache
static int ldb_filecache_pdata_set(ldb_filecache_t *cache, const char *path, const struct ldb_filecache_pdata *pdata) {
    leveldb_writeoptions_t *options;
    char *errptr = NULL;
    char *key;
    int ret = -1;

    if (!pdata) {
        log_print(LOG_ERR, "ldb_filecache_pdata_set NULL pdata");
        goto finish;
    }

    log_print(LOG_DEBUG, "ldb_filecache_pdata_set: path=%s ; cachefile=%s", path, pdata->filename);

    key = path2key(path);
    options = leveldb_writeoptions_create();
    leveldb_put(cache, options, key, strlen(key) + 1, (const char *) pdata, sizeof(struct ldb_filecache_pdata), &errptr);
    leveldb_writeoptions_destroy(options);

    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_set error: %s", errptr);
        free(errptr);
        goto finish;
    }

    ret = 0;

finish:

    return ret;
}

// Create a new file to write into and set values
static int create_file(struct ldb_filecache_sdata *sdata, const char *cache_path,
        ldb_filecache_t *cache, const char *path) {

    struct stat_cache_value value;
    struct ldb_filecache_pdata *pdata;

    log_print(LOG_DEBUG, "create_file: on %s", path);
    sdata->modified = true;
    sdata->writable = true;
    if (new_cache_file(cache_path, sdata->filename, &sdata->fd) < 0) {
        log_print(LOG_ERR, "ldb_filecache_open: Failed on new_cache_file");
        return -1;
    }

    // Prepopulate stat cache.
    value.st.st_mode = 0660 | S_IFREG;
    value.st.st_nlink = 1;
    value.st.st_size = 0;
    value.st.st_atime = time(NULL);
    value.st.st_mtime = value.st.st_atime;
    value.st.st_ctime = value.st.st_mtime;
    value.st.st_blksize = 0;
    value.st.st_blocks = 8;
    value.st.st_uid = getuid();
    value.st.st_gid = getgid();
    value.prepopulated = false;
    stat_cache_value_set(cache, path, &value);
    log_print(LOG_DEBUG, "create_file: Updated stat cache for %d : %s", sdata->fd, path);

    // Prepopulate filecache.
    pdata = malloc(sizeof(struct ldb_filecache_pdata));
    if (pdata == NULL) {
        log_print(LOG_ERR, "create_file: malloc returns NULL for pdata");
        return -1;
    }
    memset(pdata, 0, sizeof(struct ldb_filecache_pdata));
    strncpy(pdata->filename, sdata->filename, sizeof(pdata->filename));

    // The local copy currently trumps the server one, no matter how old.
    pdata->last_server_update = 0;

    log_print(LOG_DEBUG, "create_file: Updating file cache for %d : %s : %s : timestamp %ul.", sdata->fd, path, pdata->filename, pdata->last_server_update);
    ldb_filecache_pdata_set(cache, path, pdata);
    free(pdata);

    return 0;
}

// get an entry from the ldb cache
static struct ldb_filecache_pdata *ldb_filecache_pdata_get(ldb_filecache_t *cache, const char *path) {
    struct ldb_filecache_pdata *pdata = NULL;
    char *key;
    leveldb_readoptions_t *options;
    size_t vallen;
    char *errptr = NULL;

    log_print(LOG_DEBUG, "Entered ldb_filecache_pdata_get: path=%s", path);

    key = path2key(path);

    options = leveldb_readoptions_create();
    leveldb_readoptions_set_fill_cache(options, false);
    pdata = (struct ldb_filecache_pdata *) leveldb_get(cache, options, key, strlen(key) + 1, &vallen, &errptr);
    leveldb_readoptions_destroy(options);
    free(key);

    if (errptr != NULL) {
        log_print(LOG_ERR, "leveldb_get error: %s", errptr);
        free(errptr);
        return NULL;
    }

    if (!pdata) {
        log_print(LOG_DEBUG, "ldb_filecache_pdata_get miss on path: %s", path);
        return NULL;
    }

    if (vallen != sizeof(struct ldb_filecache_pdata)) {
        log_print(LOG_ERR, "Length %lu is not expected length %lu.", vallen, sizeof(struct ldb_filecache_pdata));
    }

    log_print(LOG_DEBUG, "Returning from ldb_filecache_pdata_get: path=%s :: cachefile=%s", path, pdata->filename);

    return pdata;
}

// Get a file descriptor pointing to the latest full copy of the file.
static int ldb_get_fresh_fd(ne_session *session, ldb_filecache_t *cache,
        const char *cache_path, const char *path, struct ldb_filecache_sdata *sdata,
        struct ldb_filecache_pdata **pdatap, int flags) {
    int ret = -EBADFD;
    int code;
    ne_request *req = NULL;
    int ne_ret;
    struct ldb_filecache_pdata *pdata;

    assert(pdatap);
    pdata = *pdatap;

    if (pdata != NULL)
        log_print(LOG_DEBUG, "ldb_get_fresh_fd: file found in cache: %s::%s", path, pdata->filename);

    // Is it usable as-is?
    // We should have guaranteed that if O_TRUNC is specified and pdata is NULL we don't get here.
    // For O_TRUNC, we just want to open a truncated cache file and not bother getting a copy from
    // the server.
    // If not O_TRUNC, but the cache file is fresh, just reuse it without going to the server.
    if (pdata != NULL && ( (flags & O_TRUNC) || (pdata->last_server_update == 0 || (time(NULL) - pdata->last_server_update) <= REFRESH_INTERVAL))) {
        log_print(LOG_DEBUG, "ldb_get_fresh_fd: file is fresh or being truncated: %s::%s", path, pdata->filename);

        // Open first with O_TRUNC off to avoid modifying the file without holding the right lock.
        sdata->fd = open(pdata->filename, flags & ~O_TRUNC);
        if (sdata->fd < 0) {
            // If the cachefile named in pdata->filename does not exist ...
            // @TODO If O_TRUNC, we can't make pdata NULL and try again, since we need
            // to handle pdata NULL and O_TRUNC in a different call. So for now,
            // don't allow EAGAIN on O_TRUNC, but think about fixing later
            if (errno == ENOENT && !(flags & O_TRUNC)) {
                // try again
                ret = -EAGAIN;
                log_print(LOG_NOTICE, "ldb_get_fresh_fd: ENOENT on fresh/trunc, cause retry: open for fresh/trunc on %s with flags %x returns < 0: errno: %d, %s", path, flags, errno, strerror(errno));
            }
            else {
                log_print(LOG_ERR, "ldb_get_fresh_fd: open on file returns < 0 on \"%s\": errno: %d, %s", path, errno, strerror(errno));
            }
            return ret;
        }

        if (flags & O_TRUNC) {
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: truncating fd %d:%s::%s", sdata->fd, path, pdata->filename);

            log_print(LOG_DEBUG, "ldb_get_fresh_fd: acquiring shared file lock on fd %d", sdata->fd);
            if (flock(sdata->fd, LOCK_SH)) {
                log_print(LOG_ERR, "ldb_get_fresh_fd: error acquiring shared file lock");
                goto finish;
            }
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: acquired shared file lock on fd %d", sdata->fd);

            if (ftruncate(sdata->fd, 0)) {
                log_print(LOG_WARNING, "ldb_get_fresh_fd: ftruncate failed; errno %d %s -- %d:%s::%s", errno, strerror(errno), sdata->fd, path, pdata->filename);
            }

            log_print(LOG_DEBUG, "ldb_get_fresh_fd: releasing shared file lock on fd %d", sdata->fd);
            if (flock(sdata->fd, LOCK_UN)) {
                log_print(LOG_ERR, "ldb_get_fresh_fd: error releasing shared file lock");
                goto finish;
            }
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: released shared file lock on fd %d", sdata->fd);

            sdata->modified = true;
        }
        else {
            log_print(LOG_DEBUG, "ldb_get_fresh_fd: O_TRUNC not specified on fd %d:%s::%s", sdata->fd, path, pdata->filename);
        }

        ret = 0;
        // We're done; no need to access the server...
        goto finish;
    }

    req = ne_request_create(session, "GET", path);
    if (!req) {
        log_print(LOG_ERR, "ldb_get_fresh_fd: Failed ne_request_create on GET on %s", path);
        goto finish;
    }

    // If we have stale cache data, set a header to aim for a 304.
    if (pdata)
        ne_add_request_header(req, "If-None-Match", pdata->etag);

    do {
        ne_ret = ne_begin_request(req);
        if (ne_ret != NE_OK) {
            log_print(LOG_ERR, "ldb_get_fresh_fd: ne_begin_request is not NE_OK: %d %s",
                ne_ret, ne_get_error(session));
            goto finish;
        }

        // If we get a 304, the cache file has the same contents as the file on the server, so
        // just open the cache file without bothering to re-GET the contents from the server.
        // If we get a 200, the cache file is stale and we need to update its contents from
        // the server.
        // We should not get a 404 here; either the open included O_CREAT and we create a new
        // file, or the getattr/get_stat calls in fusedav.c should have detected the file was
        // missing and handled it there.
        code = ne_get_status(req)->code;
        if (code == 304) {
            log_print(LOG_DEBUG, "Got 304 on %s with etag %s", path, pdata->etag);

            // Gobble up any remaining data in the response.
            ne_discard_response(req);

            if (pdata != NULL) {
                // Mark the cache item as revalidated at the current time.
                pdata->last_server_update = time(NULL);

                log_print(LOG_DEBUG, "ldb_get_fresh_fd: Updating file cache on 304 for %s : %s : timestamp: %ul.", path, pdata->filename, pdata->last_server_update);
                ldb_filecache_pdata_set(cache, path, pdata);

                sdata->fd = open(pdata->filename, flags);

                if (sdata->fd < 0) {
                    // If the cachefile named in pdata->filename does not exist ...
                    if (errno == ENOENT) {
                        // delete pdata from cache, we can't trust its values.
                        // We see one site continually failing on the same non-existent cache file.
                        if (ldb_filecache_delete(cache, path, true) < 0) {
                            log_print(LOG_ERR, "ldb_get_fresh_fd: ENOENT on 304 failed to delete filecache entry for %s", path);
                        }
                        else {
                            // Now that we've gotten rid of pdata
                            ret = -EAGAIN;
                            log_print(LOG_NOTICE, "ldb_get_fresh_fd: ENOENT, cause retry: open for 304 on %s with flags %x returns < 0: errno: %d, %s", path, flags, errno, strerror(errno));
                        }
                    }
                    else {
                        log_print(LOG_ERR, "ldb_get_fresh_fd: open for 304 on %s with flags %x and etag %s returns < 0: errno: %d, %s", pdata->filename, flags, pdata->etag, errno, strerror(errno));
                    }
                }
                else {
                    ret = 0;
                    log_print(LOG_DEBUG, "ldb_get_fresh_fd: open for 304 on %s with flags %x succeeded; fd %d", pdata->filename, flags, sdata->fd);
                }
            }
            else {
                log_print(LOG_WARNING, "ldb_get_fresh_fd: Got 304 without If-None-Match");
            }
        }
        else if (code == 200) {
            // Archive the old temp file path for unlinking after replacement.
            char old_filename[PATH_MAX];
            bool unlink_old = false;
            const char *etag = NULL;

            if (pdata == NULL) {
                *pdatap = malloc(sizeof(struct ldb_filecache_pdata));
                pdata = *pdatap;
                if (pdata == NULL) {
                    log_print(LOG_ERR, "ldb_get_fresh_fd: malloc returns NULL for pdata");
                    ne_end_request(req);
                    goto finish;
                }
                memset(pdata, 0, sizeof(struct ldb_filecache_pdata));
            }
            else {
                strncpy(old_filename, pdata->filename, PATH_MAX);
                unlink_old = true;
            }

            // Fill in ETag.
            etag = ne_get_response_header(req, "ETag");
            if (etag != NULL) {
                log_print(LOG_DEBUG, "Got ETag: %s", etag);
                strncpy(pdata->etag, etag, ETAG_MAX);
                pdata->etag[ETAG_MAX] = '\0'; // length of etag is ETAG_MAX + 1 to accomodate this null terminator
            }
            else {
                log_print(LOG_DEBUG, "Got no ETag in response.");
                pdata->etag[0] = '\0';
            }

            // Create a new temp file and read the file content into it.
            if (new_cache_file(cache_path, pdata->filename, &sdata->fd) < 0) {
                log_print(LOG_ERR, "ldb_get_fresh_fd: new_cache_file returns < 0");
                // Should we delete path from cache and/or null-out pdata?
                ne_end_request(req);
                goto finish;
            }
            ret = 0;
            ne_read_response_to_fd(req, sdata->fd);

            // Point the persistent cache to the new file content.
            pdata->last_server_update = time(NULL);

            log_print(LOG_DEBUG, "ldb_get_fresh_fd: Updating file cache on 200 for %s : %s : timestamp: %ul.", path, pdata->filename, pdata->last_server_update);
            ldb_filecache_pdata_set(cache, path, pdata);

            // Unlink the old cache file, which the persistent cache
            // no longer references. This will cause the file to be
            // deleted once no more file descriptors reference it.
            if (unlink_old) {
                unlink(old_filename);
                log_print(LOG_DEBUG, "ldb_get_fresh_fd: 200: unlink old filename %s", old_filename);
            }
        }
        else if (code == 404) {
            log_print(LOG_WARNING, "ldb_get_fresh_fd: File expected to exist returns 404.");
            ret = -ENOENT;
        }
        else {
            // Not sure what to do here; goto finish, or try the loop another time?
            log_print(LOG_WARNING, "ldb_get_fresh_fd: returns %d; expected 304 or 200", code);
        }

        ne_ret = ne_end_request(req);
    } while (ne_ret == NE_RETRY);

    // No check for O_TRUNC here because we skip server access and just
    // truncate.
    assert(!(flags & O_TRUNC));

    finish:
        if (req != NULL)
            ne_request_destroy(req);
        return ret;
}

// top-level open call
int ldb_filecache_open(char *cache_path, ldb_filecache_t *cache, const char *path, struct fuse_file_info *info) {
    ne_session *session;
    struct ldb_filecache_pdata *pdata = NULL;
    struct ldb_filecache_sdata *sdata = NULL;
    int ret = -EBADF;
    int flags = info->flags;

    log_print(LOG_DEBUG, "ldb_filecache_open: %s", path);

    if (!(session = session_get(1))) {
        ret = -EIO;
        log_print(LOG_ERR, "ldb_filecache_open: Failed to get session");
        goto fail;
    }

    // Allocate and zero-out a session data structure.
    sdata = malloc(sizeof(struct ldb_filecache_sdata));
    if (sdata == NULL) {
        log_print(LOG_ERR, "ldb_filecache_open: Failed to malloc sdata");
        goto fail;
    }
    memset(sdata, 0, sizeof(struct ldb_filecache_sdata));

    // If open is called twice, both times with O_CREAT, fuse does not pass O_CREAT
    // the second time. (Unlike on a linux file system, where the second time open
    // is called with O_CREAT, the flag is there but is ignored.) So O_CREAT here
    // means new file.

    // If O_TRUNC is called, it is possible that there is no entry in the filecache.
    // I believe the use-case for this is: prior to conversion to fusedav, a file
    // was on the server. After conversion to fusedav, on first access, it is not
    // in the cache, so we need to create a new cache file for it (or it has aged
    // out of the cache.) If it is in the cache, we let ldb_get_fresh_fd handle it.

    // @TODO Extend retry logic to the whole if-else, so that if we make
    // pdata NULL and we have O_TRUNC, we can start all over again at create_file
    pdata = ldb_filecache_pdata_get(cache, path);
    if ((flags & O_CREAT) || ((flags & O_TRUNC) && (pdata == NULL))) {
        if ((flags & O_CREAT) && (pdata != NULL)) {
            // This will orphan the previous filecache file
            log_print(LOG_INFO, "ldb_filecache_open: creating a file that already has a cache entry: %s", path);
        }
        ret = create_file(sdata, cache_path, cache, path);
        if (ret < 0) {
            log_print(LOG_ERR, "ldb_filecache_open: Failed on create for %s", path);
            goto fail;
        }
    }
    else {
        // Paranoid. Avoid a potential infinite loop of EAGAINs. Value of max_retries is arbitrary
        unsigned retries = 0;
        const unsigned max_retries = 2;
        // Get a file descriptor pointing to a guaranteed-fresh file.
        do {
            ++retries;
            ret = ldb_get_fresh_fd(session, cache, cache_path, path, sdata, &pdata, flags);
            if (ret == 0) {
                log_print(LOG_DEBUG, "ldb_filecache_open: success on %s", path);
            }
            else if (ret == -EAGAIN && (retries < max_retries)) {
                log_print(LOG_NOTICE, "ldb_filecache_open: Got EAGAIN on %s; try again", path);
                if (ldb_filecache_delete(cache, path, true) < 0) {
                    log_print(LOG_ERR, "ldb_get_fresh_fd: ENOENT on fresh/trunc failed to delete filecache entry for %s, cachefile %s", path, pdata->filename);
                    goto fail;
                }
                else {
                    // Now that we've gotten rid of cache entry, free pdata
                    if (pdata) free(pdata);
                    pdata = NULL;
                }
            }
            else {
                log_print(LOG_ERR, "ldb_filecache_open: Failed on ldb_get_fresh_fd on %s", path);
                goto fail;
            }
        } while (ret == -EAGAIN);
    }

    if (flags & O_RDONLY || flags & O_RDWR) sdata->readable = 1;
    if (flags & O_WRONLY || flags & O_RDWR) sdata->writable = 1;

    if (sdata->fd >= 0) {
        if (pdata) log_print(LOG_DEBUG, "ldb_filecache_open: Setting fd to session data structure with fd %d for %s :: %s:%ul.", sdata->fd, path, pdata->filename, pdata->last_server_update);
        else log_print(LOG_DEBUG, "ldb_filecache_open: Setting fd to session data structure with fd %d for %s :: (no pdata).", sdata->fd, path);
        info->fh = (uint64_t) sdata;
        ret = 0;
        goto finish;
    }

fail:
    log_print(LOG_ERR, "ldb_filecache_open: No valid fd set for path %s. Setting fh structure to NULL.", path);
    info->fh = (uint64_t) NULL;

    if (sdata != NULL)
        free(sdata);

finish:
    if (pdata != NULL)
        free(pdata);

    return ret;
}

// top-level read call
ssize_t ldb_filecache_read(struct fuse_file_info *info, char *buf, size_t size, ne_off_t offset) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    ssize_t ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_read: fd=%d", sdata->fd);

    if ((ret = pread(sdata->fd, buf, size, offset)) < 0) {
        ret = -errno;
        log_print(LOG_ERR, "ldb_filecache_read: error %d; %d %s %d %ld", ret, sdata->fd, buf, size, offset);
        goto finish;
    }

finish:

    // ret is bytes read, or error
    log_print(LOG_DEBUG, "Done reading: %d from %d.", ret, sdata->fd);

    return ret;
}

// top-level write call
ssize_t ldb_filecache_write(struct fuse_file_info *info, const char *buf, size_t size, ne_off_t offset) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    ssize_t ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_write: fd=%d", sdata->fd);

    // Don't write to a file while it is being PUT
    log_print(LOG_DEBUG, "ldb_filecache_write: acquiring shared file lock on fd %d", sdata->fd);
    if (flock(sdata->fd, LOCK_SH)) {
        log_print(LOG_ERR, "ldb_filecache_write: error acquiring shared file lock");
        // return 0 since it is the number of bytes written
        return 0;
    }
    log_print(LOG_DEBUG, "ldb_filecache_write: acquired shared file lock on fd %d", sdata->fd);

    if (!sdata->writable) {
        errno = EBADF;
        ret = 0;
        log_print(LOG_DEBUG, "ldb_filecache_write: not writable");
        goto finish;
    }

    if ((ret = pwrite(sdata->fd, buf, size, offset)) < 0) {
        ret = -errno;
        log_print(LOG_ERR, "ldb_filecache_write: error %d %d %s::%d %d %ld", ret, errno, strerror(errno), sdata->fd, size, offset);
        goto finish;
    }

    sdata->modified = true;

finish:

    log_print(LOG_DEBUG, "ldb_filecache_write: releasing shared file lock on fd %d", sdata->fd);
    if (flock(sdata->fd, LOCK_UN)) {
        log_print(LOG_ERR, "ldb_filecache_write: error releasing shared file lock");
        // Since we've already written (or not), just fall through and return ret
    }
    log_print(LOG_DEBUG, "ldb_filecache_write: released shared file lock on fd %d", sdata->fd);

    // ret is bytes written
    return ret;
}

// close the file
static int ldb_filecache_close(struct ldb_filecache_sdata *sdata) {
    int ret = -EBADF;

    log_print(LOG_DEBUG, "ldb_filecache_close: fd (%d :: %d).", sdata->fd, sdata->fd);

    if (sdata->fd > 0)  {
        if (close(sdata->fd) < 0) {
            ret = -errno;
            log_print(LOG_ERR, "ldb_filecache_close: Failed to close cache file; %d %s", ret, strerror(errno));
        }
        else {
            log_print(LOG_DEBUG, "ldb_filecache_close: closed fd (%d).", sdata->fd);
            ret = 0;
        }
    }
    else {
        log_print(LOG_ERR, "ldb_filecache_close: Session data lacks a cache file descriptor.");
    }

    if (sdata != NULL)
        free(sdata);

    return ret;
}

// top-level close/release call
int ldb_filecache_release(ldb_filecache_t *cache, const char *path, struct fuse_file_info *info) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;

    assert(sdata);

    log_print(LOG_DEBUG, "ldb_filecache_release: %s : %d", path?path:"NULL", sdata->fd);

    // If path is NULL, sync will handle it. Likely sync was already called
    // during immediately preceding flush; in that case, file won't be
    // modifiable and will return immediate.
    if ((ret = ldb_filecache_sync(cache, path, info, true)) < 0) {
        log_print(LOG_ERR, "ldb_filecache_release: ldb_filecache_sync returns error %d", ret);
        goto finish;
    }

    log_print(LOG_DEBUG, "Done syncing file (%s) for release, calling ldb_filecache_close.", path);

    ret = 0;

finish:

    // close, even on error
    ret = ldb_filecache_close(sdata);

    log_print(LOG_DEBUG, "ldb_filecache_release: Done releasing file (%s).", path?path:"NULL");

    return ret;
}

/* PUT's from fd to URI */
/* Our modification to include etag support on put */
static int ne_put_return_etag(ne_session *session, const char *path, int fd, char *etag)
{
    ne_request *req;
    struct stat st;
    int ret = -1;
    const char *value;

    log_print(LOG_DEBUG, "enter: ne_put_return_etag(,%s,%d,,)", path, fd);

    log_print(LOG_DEBUG, "ne_put_return_etag: acquiring exclusive file lock on fd %d", fd);
    if (flock(fd, LOCK_EX)) {
        log_print(LOG_ERR, "ne_put_return_etag: error acquiring exclusive file lock");
    }
    log_print(LOG_DEBUG, "ne_put_return_etag: acquired exclusive file lock on fd %d", fd);

    assert(etag);

    if (fstat(fd, &st)) {
        log_print(LOG_ERR, "ne_put_return_etag: failed getting file size");
        goto finish;
    }

    log_print(LOG_DEBUG, "ne_put_return_etag: file size %d", st.st_size);

    req = ne_request_create(session, "PUT", path);

    ne_lock_using_resource(req, path, 0);
    ne_lock_using_parent(req, path);

    ne_set_request_body_fd(req, fd, 0, st.st_size);

    ret = ne_request_dispatch(req);

    if (ret != NE_OK) {
        log_print(LOG_WARNING, "ne_put_return_etag: ne_request_dispatch returns error (%d:%s: fd=%d)", ret, ne_get_error(session), fd);
    }
    else {
        log_print(LOG_DEBUG, "ne_put_return_etag: ne_request_dispatch succeeds (fd=%d)", fd);
    }

    if (ret == NE_OK && ne_get_status(req)->klass != 2) {
        ret = NE_ERROR;
    }

    // We continue to PUT the file if etag happens to be NULL; it just
    // means ultimately that it won't trigger a 304 on next access
    if (ret == NE_OK) {
        value = ne_get_response_header(req, "etag");
        if (value) {
            strncpy(etag, value, ETAG_MAX);
            etag[ETAG_MAX] = '\0';
        }
        log_print(LOG_DEBUG, "PUT returns etag: %s", etag);
    }
    else {
        etag[0] = '\0';
    }
    ne_request_destroy(req);

finish:

    log_print(LOG_DEBUG, "ne_put_return_etag: releasing exclusive file lock on fd %d", fd);
    if (flock(fd, LOCK_UN)) {
        log_print(LOG_ERR, "ne_put_return_etag: error releasing exclusive file lock");
    }
    log_print(LOG_DEBUG, "ne_put_return_etag: released exclusive file lock on fd %d", fd);

    log_print(LOG_DEBUG, "exit: ne_put_return_etag");

    return ret;
}

// top-level sync call
int ldb_filecache_sync(ldb_filecache_t *cache, const char *path, struct fuse_file_info *info, bool do_put) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;
    struct ldb_filecache_pdata *pdata = NULL;
    ne_session *session;
    struct stat_cache_value value;

    assert(sdata);

    // We only do the sync if we have a path
    // If we are accessing a bare file descriptor (open/unlink/read|write),
    // path will be NULL, so just return without doing anything
    if (path == NULL) {
        log_print(LOG_DEBUG, "ldb_filecache_sync(NULL path, returning, fd=%d)", sdata->fd);
        return 0;
    }
    else {
        log_print(LOG_DEBUG, "ldb_filecache_sync(%s, fd=%d)", path, sdata->fd);
    }

    log_print(LOG_DEBUG, "ldb_filecache_sync: Checking if file (%s) was writable.", path);
    if (!sdata->writable) {
        ret = 0;
        log_print(LOG_DEBUG, "ldb_filecache_sync: not writable");
        goto finish;
    }

    // Write this data to the persistent cache.
    // Update the file cache
    pdata = ldb_filecache_pdata_get(cache, path);
    if (pdata == NULL) {
        log_print(LOG_ERR, "ldb_filecache_sync(%s, fd=%d): pdata is NULL", path, sdata->fd);
        goto finish;
    }
    log_print(LOG_DEBUG, "ldb_filecache_sync(%s, fd=%d): cachefile=%s", path, sdata->fd, pdata->filename);

    if (sdata->modified) {
        if (do_put) {
            log_print(LOG_DEBUG, "ldb_filecache_sync: Seeking fd=%d", sdata->fd);
            if (lseek(sdata->fd, 0, SEEK_SET) == (ne_off_t)-1) {
                log_print(LOG_ERR, "ldb_filecache_sync: failed lseek :: %d %d %s", sdata->fd, errno, strerror(errno));
                ret = -1;
                goto finish;
            }

            log_print(LOG_DEBUG, "Getting libneon session.");
            if (!(session = session_get(1))) {
                errno = EIO;
                ret = -1;
                log_print(LOG_ERR, "ldb_filecache_sync: failed session");
                goto finish;
            }

            log_print(LOG_DEBUG, "About to PUT file (%s, fd=%d).", path, sdata->fd);

            if (ne_put_return_etag(session, path, sdata->fd, pdata->etag)) {
                log_print(LOG_ERR, "ldb_filecache_sync: ne_put PUT failed: %s: fd=%d", ne_get_error(session), sdata->fd);
                errno = ENOENT;
                ret = -1;
                goto finish;
            }

            log_print(LOG_DEBUG, "ldb_filecache_sync: PUT successful: %s : %s : old-timestamp: %ul: etag = %s", path, pdata->filename, pdata->last_server_update, pdata->etag);

            // If the PUT succeeded, the file isn't locally modified.
            sdata->modified = false;
            pdata->last_server_update = time(NULL);
        }
        else {
            // If we don't PUT the file, we don't have an etag, so zero it out
            strncpy(pdata->etag, "", 1);

            // The local copy currently trumps the server one, no matter how old.
            pdata->last_server_update = 0;
        }
    }

    // Point the persistent cache to the new file content.
    ldb_filecache_pdata_set(cache, path, pdata);

    // Update stat cache.
    // @TODO: Use actual mode.
    value.st.st_mode = 0660 | S_IFREG;
    value.st.st_nlink = 1;
    value.st.st_size = lseek(sdata->fd, 0, SEEK_END);
    value.st.st_atime = time(NULL);
    value.st.st_mtime = value.st.st_atime;
    value.st.st_ctime = value.st.st_mtime;
    value.st.st_blksize = 0;
    value.st.st_blocks = 8;
    value.st.st_uid = getuid();
    value.st.st_gid = getgid();
    value.prepopulated = false;
    stat_cache_value_set(cache, path, &value);
    log_print(LOG_DEBUG, "ldb_filecache_sync: Updated stat cache %d:%s:%s:%ul", sdata->fd, path, pdata->filename, pdata->last_server_update);

    ret = 0;

finish:

    if (pdata) free(pdata);

    log_print(LOG_DEBUG, "ldb_filecache_sync: Done syncing file (%s, fd=%d).", path, sdata->fd);

    return ret;
}

// top-level truncate call
int ldb_filecache_truncate(struct fuse_file_info *info, ne_off_t s) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;
    int ret = -1;

    log_print(LOG_DEBUG, "ldb_filecache_truncate(%d)", sdata->fd);

    log_print(LOG_DEBUG, "ldb_filecache_truncate: acquiring shared file lock on fd %d", sdata->fd);
    if (flock(sdata->fd, LOCK_SH)) {
        log_print(LOG_ERR, "ldb_filecache_truncate: error acquiring shared file lock");
        return ret;
    }
    log_print(LOG_DEBUG, "ldb_filecache_truncate: acquired shared file lock on fd %d", sdata->fd);

    if ((ret = ftruncate(sdata->fd, s)) < 0) {
        log_print(LOG_ERR, "ldb_filecache_truncate: error on ftruncate %d", ret);
    }

    log_print(LOG_DEBUG, "ldb_filecache_truncate: releasing shared file lock on fd %d", sdata->fd);
    if (flock(sdata->fd, LOCK_UN)) {
        log_print(LOG_ERR, "ldb_filecache_truncate: error releasing shared file lock");
    }
    log_print(LOG_DEBUG, "ldb_filecache_truncate: released shared file lock on fd %d", sdata->fd);

    sdata->modified = true;

    return ret;
}

int ldb_filecache_fd(struct fuse_file_info *info) {
    struct ldb_filecache_sdata *sdata = (struct ldb_filecache_sdata *)info->fh;

    log_print(LOG_DEBUG, "ldb_filecache_fd: %d", sdata->fd);
    return sdata->fd;
}

// deletes entry from ldb cache
int ldb_filecache_delete(ldb_filecache_t *cache, const char *path, bool unlink_cachefile) {
    struct ldb_filecache_pdata *pdata = NULL;
    leveldb_writeoptions_t *options;
    char *key;
    int ret = 0;
    char *errptr = NULL;

    log_print(LOG_DEBUG, "ldb_filecache_delete: path (%s).", path);

    pdata = ldb_filecache_pdata_get(cache, path);

    key = path2key(path);

    options = leveldb_writeoptions_create();
    leveldb_delete(cache, options, key, strlen(key) + 1, &errptr);
    leveldb_writeoptions_destroy(options);
    free(key);

    if (unlink_cachefile && pdata) {
        log_print(LOG_DEBUG, "ldb_filecache_delete: unlinking %s", pdata->filename);
        if (unlink(pdata->filename)) {
            log_print(LOG_DEBUG, "ldb_filecache_delete: error unlinking %s", pdata->filename);
        }
    }

    if (errptr != NULL) {
        log_print(LOG_ERR, "ERROR: leveldb_delete: %s", errptr);
        free(errptr);
        ret = -1;
    }

    if (pdata) free(pdata);

    return ret;
}

int ldb_filecache_pdata_move(ldb_filecache_t *cache, const char *old_path, const char *new_path) {
    struct ldb_filecache_pdata *pdata = NULL;
    int ret = -1;

    pdata = ldb_filecache_pdata_get(cache, old_path);

    if (pdata == NULL) {
        log_print(LOG_NOTICE, "ldb_filecache_pdata_move: Path %s does not exist.", old_path);
        goto finish;
    }

    log_print(LOG_DEBUG, "ldb_filecache_pdata_move: Update last_server_update on %s: timestamp: %ul", pdata->filename, pdata->last_server_update);

    if (ldb_filecache_pdata_set(cache, new_path, pdata) < 0) {
        log_print(LOG_ERR, "ldb_filecache_pdata_move: Moving entry from path %s to %s failed. Could not write new entry.", old_path, new_path);
        goto finish;
    }

    // We don't want to unlink the cachefile for 'old' since we use it for 'new'
    ldb_filecache_delete(cache, old_path, false);

    ret = 0;

    log_print(LOG_DEBUG, "ldb_filecache_pdata_move: new cachefile is %s", pdata->filename);

finish:

    if (pdata) free(pdata);

    return ret;
}
