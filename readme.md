so # Mini-UnionFS Design Document

## 1. Overview

Mini-UnionFS is a userspace union filesystem implemented using FUSE (Filesystem in Userspace). It presents a unified view of two directory layers: a read-only **lower layer** (representing a base image) and a read-write **upper layer** (representing a container's writable layer). This design mirrors the layered filesystem approach used by container runtimes like Docker.

## 2. Architecture

### 2.1 Layer Model

```
┌─────────────────────────────────────┐
│         Mounted View (mnt)          │  ← User sees unified filesystem
├─────────────────────────────────────┤
│    FUSE (mini_unionfs process)      │  ← Routes requests, manages CoW
├──────────────────┬──────────────────┤
│  Upper Layer     │   Lower Layer    │
│  (read-write)    │   (read-only)    │
│                  │                  │
│  - New files     │  - Base files    │
│  - Modified      │  - Original      │
│  - Whiteouts     │    content       │
└──────────────────┴──────────────────┘
```

### 2.2 Data Structures

#### Global State
```c
struct mini_unionfs_state {
    char *lower_dir;  // Absolute path to lower (read-only) directory
    char *upper_dir;  // Absolute path to upper (read-write) directory
};
```

#### Path Resolution Result
```c
typedef enum {
    PATH_NONE,      // File doesn't exist anywhere
    PATH_LOWER,     // File exists only in lower layer
    PATH_UPPER,     // File exists in upper layer (takes precedence)
    PATH_WHITEOUT   // File has been deleted (whiteout exists)
} path_location_t;
```

## 3. Core Algorithms

### 3.1 Path Resolution

The `resolve_path()` function determines where a file exists:

1. Check if a whiteout file (`.wh.<filename>`) exists in upper directory
   - If yes: return `PATH_WHITEOUT` (file is "deleted")
2. Check if file exists in upper directory
   - If yes: return `PATH_UPPER` with the upper path
3. Check if file exists in lower directory
   - If yes: return `PATH_LOWER` with the lower path
4. Otherwise: return `PATH_NONE`

**Precedence Rule**: Upper layer always takes precedence over lower layer.

### 3.2 Copy-on-Write (CoW)

When a write operation targets a file that exists only in the lower layer:

1. Create parent directories in upper layer (preserving permissions from lower)
2. Copy entire file content from lower to upper
3. Copy file permissions and ownership
4. Perform the write operation on the upper copy
5. Lower layer file remains untouched

**Trigger Points**: CoW is triggered in `open()` when flags include `O_WRONLY`, `O_RDWR`, or `O_APPEND`.

### 3.3 Whiteout Mechanism

When deleting a file that exists in the lower layer:

1. If file exists in upper layer: physically delete it
2. Create a whiteout marker: `upper_dir/.wh.<filename>`
3. The whiteout is an empty file that signals "this file is deleted"

**Whiteout Interpretation**:
- `getattr()`: Returns `-ENOENT` for whiteouted files
- `readdir()`: Filters out whiteouted entries and whiteout files themselves
- `create()`: Removes any existing whiteout to "resurrect" the file

### 3.4 Directory Listing Merge

The `readdir()` operation merges both layers:

1. Read upper directory entries
   - Track seen filenames
   - Track whiteout files (extract original names)
   - Add non-whiteout entries to result
2. Read lower directory entries
   - Skip if already seen in upper (upper takes precedence)
   - Skip if whiteouted
   - Add remaining entries to result
3. Never expose `.wh.*` files to user

## 4. POSIX Operations Implementation

| Operation | Behavior |
|-----------|----------|
| `getattr` | Resolve path, return stats from resolved location (or ENOENT if whiteouted) |
| `readdir` | Merge upper and lower directories, filtering whiteouts |
| `open`    | Resolve path; trigger CoW if writing to lower file |
| `read`    | Read from resolved path location |
| `write`   | Write to upper layer (CoW ensures file is there) |
| `create`  | Create in upper layer, remove any existing whiteout |
| `unlink`  | Delete from upper if present; create whiteout if in lower |
| `mkdir`   | Create in upper layer, remove any existing whiteout |
| `rmdir`   | Remove from upper if present; create whiteout if in lower |
| `truncate`| CoW if needed, then truncate in upper |
| `rename`  | CoW source if in lower, rename in upper, whiteout old location |
| `chmod`   | CoW if needed, then chmod in upper |
| `chown`   | CoW if needed, then chown in upper |

## 5. Edge Cases and Handling

### 5.1 Nested Directory Creation

When creating a file in a deeply nested path that exists in lower but not upper:
- Recursively create parent directories in upper
- Preserve directory permissions from lower layer
- Handle the case where some parents exist in upper, some don't

### 5.2 File in Upper, Deleted, Then Recreated

1. File exists in lower: `/lower/config.txt`
2. User modifies it: CoW creates `/upper/config.txt`
3. User deletes it: Creates `/upper/.wh.config.txt`, removes `/upper/config.txt`
4. User creates new file: Removes whiteout, creates fresh `/upper/config.txt`

### 5.3 Directory Whiteouts

When a directory in lower layer is deleted:
- Cannot use directory whiteout to block all nested files individually
- Create single whiteout file `.wh.<dirname>` in upper
- `readdir` interprets this as the directory being deleted
- All nested files become inaccessible without individual whiteouts

### 5.4 Concurrent Access

Current implementation is single-threaded (FUSE default with `-f` flag). For production use:
- Add mutex protection around state modifications
- Consider per-file locking for CoW operations
- Handle race conditions in whiteout creation

### 5.5 Hard Links and Symlinks

Current implementation does not handle:
- Hard links across layers (would require inode mapping)
- Symlinks (could be added with `readlink` and `symlink` operations)

## 6. Limitations

1. **No Extended Attributes**: `xattr` operations not implemented
2. **No Inode Persistence**: Inode numbers may change across mounts
3. **Single Writer**: No concurrent write protection
4. **No Quota Support**: No disk usage limits per layer
5. **Memory for Large Directories**: Whiteout tracking uses dynamic memory

## 7. Usage

```bash
# Build
make

# Mount
./mini_unionfs /path/to/lower /path/to/upper /path/to/mountpoint

# Unmount
fusermount -u /path/to/mountpoint
```

## 8. Testing

The provided `test_unionfs.sh` script validates:
- Layer visibility (lower files visible through mount)
- Upper layer precedence
- Copy-on-Write behavior
- Whiteout creation and interpretation
- Directory operations
- Nested file access

Run with: `make test` or `./test_unionfs.sh`
