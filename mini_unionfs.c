/*
 * Mini-UnionFS - A simplified Union File System using FUSE
 * 
 * This implements a union filesystem with:
 * - Layer stacking (lower read-only + upper read-write)
 * - Copy-on-Write (CoW) for modifications
 * - Whiteout files for deletions
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>

/* Whiteout file prefix */
#define WHITEOUT_PREFIX ".wh."

/* Global state for the union filesystem */
struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct mini_unionfs_state *) fuse_get_context()->private_data)

/* Path resolution result */
typedef enum {
    PATH_NONE,      /* File doesn't exist */
    PATH_LOWER,     /* File exists in lower layer */
    PATH_UPPER,     /* File exists in upper layer */
    PATH_WHITEOUT   /* File is whiteouted */
} path_location_t;

/*
 * Get the whiteout filename for a given path
 * e.g., "/dir/file.txt" -> ".wh.file.txt"
 */
static void get_whiteout_name(const char *path, char *whiteout_name, size_t size) {
    char *path_copy = strdup(path);
    char *base = basename(path_copy);
    snprintf(whiteout_name, size, "%s%s", WHITEOUT_PREFIX, base);
    free(path_copy);
}

/*
 * Build the full whiteout path in upper directory
 */
static void get_whiteout_path(const char *path, char *whiteout_path, size_t size) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    char whiteout_name[PATH_MAX];
    
    get_whiteout_name(path, whiteout_name, sizeof(whiteout_name));
    
    if (strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0) {
        snprintf(whiteout_path, size, "%s/%s", state->upper_dir, whiteout_name);
    } else {
        snprintf(whiteout_path, size, "%s%s/%s", state->upper_dir, dir, whiteout_name);
    }
    free(path_copy);
}

/*
 * Check if a file is a whiteout file
 */
static int is_whiteout_file(const char *name) {
    return strncmp(name, WHITEOUT_PREFIX, strlen(WHITEOUT_PREFIX)) == 0;
}

/*
 * Check if a whiteout exists for the given path
 */
static int has_whiteout(const char *path) {
    char whiteout_path[PATH_MAX];
    struct stat st;
    
    get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
    return (lstat(whiteout_path, &st) == 0);
}

/*
 * Resolve a virtual path to its actual location
 * Returns the location type and fills resolved_path with the actual path
 */
static path_location_t resolve_path(const char *path, char *resolved_path, size_t size) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    struct stat st;
    
    /* Check for whiteout first */
    if (has_whiteout(path)) {
        return PATH_WHITEOUT;
    }
    
    /* Check upper directory */
    snprintf(resolved_path, size, "%s%s", state->upper_dir, path);
    if (lstat(resolved_path, &st) == 0) {
        return PATH_UPPER;
    }
    
    /* Check lower directory */
    snprintf(resolved_path, size, "%s%s", state->lower_dir, path);
    if (lstat(resolved_path, &st) == 0) {
        return PATH_LOWER;
    }
    
    return PATH_NONE;
}

/*
 * Build path in upper directory
 */
static void get_upper_path(const char *path, char *upper_path, size_t size) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    snprintf(upper_path, size, "%s%s", state->upper_dir, path);
}

/*
 * Build path in lower directory
 */
static void get_lower_path(const char *path, char *lower_path, size_t size) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    snprintf(lower_path, size, "%s%s", state->lower_dir, path);
}

/*
 * Ensure parent directories exist in upper layer
 */
static int ensure_parent_dirs(const char *path) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    char upper_parent[PATH_MAX];
    char lower_parent[PATH_MAX];
    struct stat st;
    int res = 0;
    
    if (strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0) {
        free(path_copy);
        return 0;
    }
    
    snprintf(upper_parent, sizeof(upper_parent), "%s%s", state->upper_dir, dir);
    
    /* Check if parent exists in upper */
    if (lstat(upper_parent, &st) == 0) {
        free(path_copy);
        return 0;
    }
    
    /* Recursively ensure grandparent exists */
    res = ensure_parent_dirs(dir);
    if (res != 0) {
        free(path_copy);
        return res;
    }
    
    /* Check if parent exists in lower to get permissions */
    snprintf(lower_parent, sizeof(lower_parent), "%s%s", state->lower_dir, dir);
    if (lstat(lower_parent, &st) == 0) {
        /* Create with same permissions as lower */
        res = mkdir(upper_parent, st.st_mode);
    } else {
        /* Create with default permissions */
        res = mkdir(upper_parent, 0755);
    }
    
    free(path_copy);
    return (res == 0 || errno == EEXIST) ? 0 : -errno;
}

/*
 * Copy a file from lower to upper layer (Copy-on-Write)
 */
static int copy_to_upper(const char *path) {
    char lower_path[PATH_MAX];
    char upper_path[PATH_MAX];
    int src_fd, dst_fd;
    struct stat st;
    char buf[8192];
    ssize_t nread;
    int res;
    
    get_lower_path(path, lower_path, sizeof(lower_path));
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    /* Get source file info */
    if (lstat(lower_path, &st) != 0) {
        return -errno;
    }
    
    /* Ensure parent directories exist */
    res = ensure_parent_dirs(path);
    if (res != 0) {
        return res;
    }
    
    /* Open source */
    src_fd = open(lower_path, O_RDONLY);
    if (src_fd < 0) {
        return -errno;
    }
    
    /* Create destination with same permissions */
    dst_fd = open(upper_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst_fd < 0) {
        close(src_fd);
        return -errno;
    }
    
    /* Copy contents */
    while ((nread = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t nwritten = write(dst_fd, buf, nread);
        if (nwritten != nread) {
            close(src_fd);
            close(dst_fd);
            return -EIO;
        }
    }
    
    /* Preserve ownership (if possible) */
    fchown(dst_fd, st.st_uid, st.st_gid);
    
    close(src_fd);
    close(dst_fd);
    
    return 0;
}

/*
 * FUSE operation: getattr - get file attributes
 */
static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi) {
    (void) fi;
    char resolved_path[PATH_MAX];
    path_location_t loc;
    
    memset(stbuf, 0, sizeof(struct stat));
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    switch (loc) {
        case PATH_WHITEOUT:
        case PATH_NONE:
            return -ENOENT;
        case PATH_UPPER:
        case PATH_LOWER:
            if (lstat(resolved_path, stbuf) != 0) {
                return -errno;
            }
            return 0;
    }
    
    return -ENOENT;
}

/*
 * FUSE operation: readdir - list directory contents
 */
static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;
    
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char lower_path[PATH_MAX];
    char upper_path[PATH_MAX];
    DIR *dp;
    struct dirent *de;
    
    /* Track seen entries to avoid duplicates and handle whiteouts */
    char **seen = NULL;
    size_t seen_count = 0;
    char **whiteouts = NULL;
    size_t whiteout_count = 0;
    
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    /* First, read upper directory */
    snprintf(upper_path, sizeof(upper_path), "%s%s", state->upper_dir, path);
    dp = opendir(upper_path);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            
            /* Track whiteout files */
            if (is_whiteout_file(de->d_name)) {
                whiteouts = realloc(whiteouts, (whiteout_count + 1) * sizeof(char *));
                /* Store the original filename (without .wh. prefix) */
                whiteouts[whiteout_count] = strdup(de->d_name + strlen(WHITEOUT_PREFIX));
                whiteout_count++;
                continue;
            }
            
            /* Add to seen list */
            seen = realloc(seen, (seen_count + 1) * sizeof(char *));
            seen[seen_count] = strdup(de->d_name);
            seen_count++;
            
            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }
    
    /* Then, read lower directory */
    snprintf(lower_path, sizeof(lower_path), "%s%s", state->lower_dir, path);
    dp = opendir(lower_path);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            
            /* Skip whiteout files in lower (shouldn't exist, but be safe) */
            if (is_whiteout_file(de->d_name)) {
                continue;
            }
            
            /* Check if whiteouted */
            int whiteouted = 0;
            for (size_t i = 0; i < whiteout_count; i++) {
                if (strcmp(de->d_name, whiteouts[i]) == 0) {
                    whiteouted = 1;
                    break;
                }
            }
            if (whiteouted) {
                continue;
            }
            
            /* Check if already seen (exists in upper) */
            int already_seen = 0;
            for (size_t i = 0; i < seen_count; i++) {
                if (strcmp(de->d_name, seen[i]) == 0) {
                    already_seen = 1;
                    break;
                }
            }
            if (already_seen) {
                continue;
            }
            
            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }
    
    /* Cleanup */
    for (size_t i = 0; i < seen_count; i++) {
        free(seen[i]);
    }
    free(seen);
    for (size_t i = 0; i < whiteout_count; i++) {
        free(whiteouts[i]);
    }
    free(whiteouts);
    
    return 0;
}

/*
 * FUSE operation: open - open a file
 */
static int unionfs_open(const char *path, struct fuse_file_info *fi) {
    char resolved_path[PATH_MAX];
    path_location_t loc;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    /* If writing to a file in lower layer, perform CoW */
    if (loc == PATH_LOWER && (fi->flags & (O_WRONLY | O_RDWR | O_APPEND))) {
        res = copy_to_upper(path);
        if (res != 0) {
            return res;
        }
        /* Update resolved path to upper */
        get_upper_path(path, resolved_path, sizeof(resolved_path));
    }
    
    /* Verify we can open the file */
    int fd = open(resolved_path, fi->flags);
    if (fd < 0) {
        return -errno;
    }
    close(fd);
    
    return 0;
}

/*
 * FUSE operation: read - read from a file
 */
static int unionfs_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi) {
    (void) fi;
    char resolved_path[PATH_MAX];
    path_location_t loc;
    int fd;
    ssize_t res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    fd = open(resolved_path, O_RDONLY);
    if (fd < 0) {
        return -errno;
    }
    
    res = pread(fd, buf, size, offset);
    if (res < 0) {
        res = -errno;
    }
    
    close(fd);
    return res;
}

/*
 * FUSE operation: write - write to a file
 */
static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    char upper_path[PATH_MAX];
    char resolved_path[PATH_MAX];
    path_location_t loc;
    int fd;
    ssize_t res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    /* If file is in lower, CoW should have happened in open() */
    /* But handle direct write case too */
    if (loc == PATH_LOWER) {
        res = copy_to_upper(path);
        if (res != 0) {
            return res;
        }
    } else if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    fd = open(upper_path, O_WRONLY);
    if (fd < 0) {
        return -errno;
    }
    
    res = pwrite(fd, buf, size, offset);
    if (res < 0) {
        res = -errno;
    }
    
    close(fd);
    return res;
}

/*
 * FUSE operation: create - create and open a file
 */
static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi) {
    (void) fi;
    char upper_path[PATH_MAX];
    char whiteout_path[PATH_MAX];
    int fd;
    int res;
    
    /* Ensure parent directories exist */
    res = ensure_parent_dirs(path);
    if (res != 0) {
        return res;
    }
    
    /* Remove any existing whiteout */
    get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
    unlink(whiteout_path);  /* Ignore errors */
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    fd = open(upper_path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) {
        return -errno;
    }
    close(fd);
    
    return 0;
}

/*
 * FUSE operation: unlink - delete a file
 */
static int unionfs_unlink(const char *path) {
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];
    char whiteout_path[PATH_MAX];
    path_location_t loc;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    if (loc == PATH_UPPER) {
        /* File exists in upper - remove it */
        res = unlink(upper_path);
        if (res != 0) {
            return -errno;
        }
        
        /* Check if file also exists in lower - if so, create whiteout */
        char lower_path[PATH_MAX];
        struct stat st;
        get_lower_path(path, lower_path, sizeof(lower_path));
        if (lstat(lower_path, &st) == 0) {
            /* Create whiteout */
            res = ensure_parent_dirs(path);
            if (res != 0) {
                return res;
            }
            get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
            int fd = open(whiteout_path, O_CREAT | O_WRONLY, 0644);
            if (fd < 0) {
                return -errno;
            }
            close(fd);
        }
    } else {
        /* File only in lower - create whiteout */
        res = ensure_parent_dirs(path);
        if (res != 0) {
            return res;
        }
        get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
        int fd = open(whiteout_path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            return -errno;
        }
        close(fd);
    }
    
    return 0;
}

/*
 * FUSE operation: mkdir - create a directory
 */
static int unionfs_mkdir(const char *path, mode_t mode) {
    char upper_path[PATH_MAX];
    char whiteout_path[PATH_MAX];
    int res;
    
    /* Ensure parent directories exist */
    res = ensure_parent_dirs(path);
    if (res != 0) {
        return res;
    }
    
    /* Remove any existing whiteout */
    get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
    unlink(whiteout_path);  /* Ignore errors */
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    res = mkdir(upper_path, mode);
    if (res != 0) {
        return -errno;
    }
    
    return 0;
}

/*
 * Helper: Remove all whiteout files from a directory in upper layer
 * This is needed when rmdir is called on a directory that only contains whiteouts
 */
static int remove_whiteouts_in_dir(const char *upper_dir_path) {
    DIR *dp;
    struct dirent *de;
    char file_path[PATH_MAX];
    
    dp = opendir(upper_dir_path);
    if (dp == NULL) {
        return 0;  /* Directory doesn't exist, that's fine */
    }
    
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        
        /* Only remove whiteout files */
        if (is_whiteout_file(de->d_name)) {
            snprintf(file_path, sizeof(file_path), "%s/%s", upper_dir_path, de->d_name);
            unlink(file_path);
        }
    }
    
    closedir(dp);
    return 0;
}

/*
 * FUSE operation: rmdir - remove a directory
 */
static int unionfs_rmdir(const char *path) {
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    char whiteout_path[PATH_MAX];
    path_location_t loc;
    struct stat st;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    get_lower_path(path, lower_path, sizeof(lower_path));
    
    if (loc == PATH_UPPER) {
        /* Remove any whiteout files inside this directory first */
        remove_whiteouts_in_dir(upper_path);
        
        /* Directory exists in upper - remove it */
        res = rmdir(upper_path);
        if (res != 0) {
            return -errno;
        }
        
        /* Check if directory also exists in lower - if so, create whiteout */
        if (lstat(lower_path, &st) == 0) {
            res = ensure_parent_dirs(path);
            if (res != 0) {
                return res;
            }
            get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
            int fd = open(whiteout_path, O_CREAT | O_WRONLY, 0644);
            if (fd < 0) {
                return -errno;
            }
            close(fd);
        }
    } else {
        /* Directory only in lower - create whiteout */
        res = ensure_parent_dirs(path);
        if (res != 0) {
            return res;
        }
        get_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
        int fd = open(whiteout_path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            return -errno;
        }
        close(fd);
    }
    
    return 0;
}

/*
 * FUSE operation: truncate - change file size
 */
static int unionfs_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi) {
    (void) fi;
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];
    path_location_t loc;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    /* If file is in lower, perform CoW first */
    if (loc == PATH_LOWER) {
        res = copy_to_upper(path);
        if (res != 0) {
            return res;
        }
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    res = truncate(upper_path, size);
    if (res != 0) {
        return -errno;
    }
    
    return 0;
}

/*
 * FUSE operation: utimens - change file timestamps
 */
static int unionfs_utimens(const char *path, const struct timespec ts[2],
                           struct fuse_file_info *fi) {
    (void) fi;
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];
    path_location_t loc;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    /* If file is in lower, perform CoW first */
    if (loc == PATH_LOWER) {
        res = copy_to_upper(path);
        if (res != 0) {
            return res;
        }
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    res = utimensat(AT_FDCWD, upper_path, ts, AT_SYMLINK_NOFOLLOW);
    if (res != 0) {
        return -errno;
    }
    
    return 0;
}

/*
 * FUSE operation: chmod - change file permissions
 */
static int unionfs_chmod(const char *path, mode_t mode,
                         struct fuse_file_info *fi) {
    (void) fi;
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];
    path_location_t loc;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    /* If file is in lower, perform CoW first */
    if (loc == PATH_LOWER) {
        res = copy_to_upper(path);
        if (res != 0) {
            return res;
        }
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    res = chmod(upper_path, mode);
    if (res != 0) {
        return -errno;
    }
    
    return 0;
}

/*
 * FUSE operation: chown - change file ownership
 */
static int unionfs_chown(const char *path, uid_t uid, gid_t gid,
                         struct fuse_file_info *fi) {
    (void) fi;
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];
    path_location_t loc;
    int res;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    /* If file is in lower, perform CoW first */
    if (loc == PATH_LOWER) {
        res = copy_to_upper(path);
        if (res != 0) {
            return res;
        }
    }
    
    get_upper_path(path, upper_path, sizeof(upper_path));
    
    res = lchown(upper_path, uid, gid);
    if (res != 0) {
        return -errno;
    }
    
    return 0;
}

/*
 * FUSE operation: rename - rename a file or directory
 */
static int unionfs_rename(const char *from, const char *to, unsigned int flags) {
    (void) flags;
    char from_resolved[PATH_MAX];
    char to_upper[PATH_MAX];
    char from_upper[PATH_MAX];
    char whiteout_path[PATH_MAX];
    path_location_t from_loc;
    int res;
    
    from_loc = resolve_path(from, from_resolved, sizeof(from_resolved));
    
    if (from_loc == PATH_WHITEOUT || from_loc == PATH_NONE) {
        return -ENOENT;
    }
    
    /* Ensure parent of destination exists */
    res = ensure_parent_dirs(to);
    if (res != 0) {
        return res;
    }
    
    get_upper_path(from, from_upper, sizeof(from_upper));
    get_upper_path(to, to_upper, sizeof(to_upper));
    
    /* If source is in lower, copy it first */
    if (from_loc == PATH_LOWER) {
        res = copy_to_upper(from);
        if (res != 0) {
            return res;
        }
    }
    
    /* Perform the rename in upper */
    res = rename(from_upper, to_upper);
    if (res != 0) {
        return -errno;
    }
    
    /* If source was in lower, create whiteout */
    if (from_loc == PATH_LOWER) {
        res = ensure_parent_dirs(from);
        if (res != 0) {
            return res;
        }
        get_whiteout_path(from, whiteout_path, sizeof(whiteout_path));
        int fd = open(whiteout_path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            return -errno;
        }
        close(fd);
    }
    
    /* Remove any whiteout at destination */
    get_whiteout_path(to, whiteout_path, sizeof(whiteout_path));
    unlink(whiteout_path);  /* Ignore errors */
    
    return 0;
}

/*
 * FUSE operation: access - check file access permissions
 */
static int unionfs_access(const char *path, int mask) {
    char resolved_path[PATH_MAX];
    path_location_t loc;
    
    loc = resolve_path(path, resolved_path, sizeof(resolved_path));
    
    if (loc == PATH_WHITEOUT || loc == PATH_NONE) {
        return -ENOENT;
    }
    
    if (access(resolved_path, mask) != 0) {
        return -errno;
    }
    
    return 0;
}

/*
 * FUSE operations structure
 */
static struct fuse_operations unionfs_oper = {
    .getattr    = unionfs_getattr,
    .readdir    = unionfs_readdir,
    .open       = unionfs_open,
    .read       = unionfs_read,
    .write      = unionfs_write,
    .create     = unionfs_create,
    .unlink     = unionfs_unlink,
    .mkdir      = unionfs_mkdir,
    .rmdir      = unionfs_rmdir,
    .truncate   = unionfs_truncate,
    .utimens    = unionfs_utimens,
    .chmod      = unionfs_chmod,
    .chown      = unionfs_chown,
    .rename     = unionfs_rename,
    .access     = unionfs_access,
};

static void usage(const char *progname) {
    fprintf(stderr, "Usage: %s <lower_dir> <upper_dir> <mount_point> [FUSE options]\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Mini-UnionFS: A simplified union filesystem using FUSE\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  lower_dir    Read-only base layer directory\n");
    fprintf(stderr, "  upper_dir    Read-write overlay directory\n");
    fprintf(stderr, "  mount_point  Where to mount the union filesystem\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s /base /overlay /mnt/union\n", progname);
}

int main(int argc, char *argv[]) {
    struct mini_unionfs_state *state;
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    int i;
    
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }
    
    state = malloc(sizeof(struct mini_unionfs_state));
    if (state == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory\n");
        return 1;
    }
    
    /* Get absolute paths for lower and upper directories */
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);
    
    if (state->lower_dir == NULL) {
        fprintf(stderr, "Error: Lower directory '%s' does not exist\n", argv[1]);
        free(state);
        return 1;
    }
    
    if (state->upper_dir == NULL) {
        fprintf(stderr, "Error: Upper directory '%s' does not exist\n", argv[2]);
        free(state->lower_dir);
        free(state);
        return 1;
    }
    
    /* Build FUSE arguments: program name, mount point, and any additional args */
    fuse_opt_add_arg(&args, argv[0]);
    fuse_opt_add_arg(&args, argv[3]);  /* mount point */
    
    /* Add any additional FUSE options */
    for (i = 4; i < argc; i++) {
        fuse_opt_add_arg(&args, argv[i]);
    }
    
    /* Run in foreground by default for debugging (remove -f for daemon mode) */
    fuse_opt_add_arg(&args, "-f");
    
    printf("Mini-UnionFS starting...\n");
    printf("  Lower (read-only):  %s\n", state->lower_dir);
    printf("  Upper (read-write): %s\n", state->upper_dir);
    printf("  Mount point:        %s\n", argv[3]);
    
    int ret = fuse_main(args.argc, args.argv, &unionfs_oper, state);
    
    fuse_opt_free_args(&args);
    free(state->lower_dir);
    free(state->upper_dir);
    free(state);
    
    return ret;
}
