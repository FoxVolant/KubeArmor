/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2023 Authors of KubeArmor */

#ifndef __SHARED_H
#define __SHARED_H

#include "vmlinux.h"
#include "vmlinux_macro.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";
#define EPERM 13

#define MAX_BUFFER_SIZE 32768
#define MAX_STRING_SIZE 256
#define MAX_BUFFERS 1
#define PATH_BUFFER 0

enum file_hook_type { dpath = 0, dfileread, dfilewrite };

enum deny_by_default {
  dproc = 101,
  dfile,
  dnet
}; // check if the list is whitelist/blacklist
enum network_check_type {
  sock_type = 2,
  sock_proto
}; // configure to check for network protocol or socket type

typedef struct buffers {
  char buf[MAX_BUFFER_SIZE];
} bufs_t;

typedef struct bufkey {
  char path[MAX_STRING_SIZE];
  char source[MAX_STRING_SIZE];
} bufs_k;

#undef container_of
#define container_of(ptr, type, member)                                        \
  ({                                                                           \
    const typeof(((type *)0)->member) *__mptr = (ptr);                         \
    (type *)((char *)__mptr - offsetof(type, member));                         \
  })
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __type(key, u32);
  __type(value, bufs_t);
  __uint(max_entries, MAX_BUFFERS);
} bufs SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __type(key, u32);
  __type(value, u32);
  __uint(max_entries, MAX_BUFFERS);
} bufs_off SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __type(key, u32);
  __type(value, bufs_k);
  __uint(max_entries, 3);
} bufk SEC(".maps");

struct outer_key {
  u32 pid_ns;
  u32 mnt_ns;
};

#define RULE_EXEC 1 << 0
#define RULE_WRITE 1 << 1
#define RULE_READ 1 << 2
#define RULE_OWNER 1 << 3
#define RULE_DIR 1 << 4
#define RULE_RECURSIVE 1 << 5
#define RULE_HINT 1 << 6
#define RULE_DENY 1 << 7

#define MASK_WRITE 0x00000002
#define MASK_READ 0x00000004
#define MASK_APPEND 0x00000008

struct data_t {
  u8 processmask;
  u8 filemask;
};

struct outer_hash {
  __uint(type, BPF_MAP_TYPE_HASH_OF_MAPS);
  __uint(max_entries, 256);
  __uint(key_size, sizeof(struct outer_key));
  __uint(value_size, sizeof(u32));
  __uint(pinning, LIBBPF_PIN_BY_NAME);
};

struct outer_hash kubearmor_containers SEC(".maps");

static __always_inline bufs_t *get_buf(int idx) {
  return bpf_map_lookup_elem(&bufs, &idx);
}

static __always_inline void set_buf_off(int buf_idx, u32 new_off) {
  bpf_map_update_elem(&bufs_off, &buf_idx, &new_off, BPF_ANY);
}

static __always_inline u32 *get_buf_off(int buf_idx) {
  return bpf_map_lookup_elem(&bufs_off, &buf_idx);
}

static inline struct mount *real_mount(struct vfsmount *mnt) {
  return container_of(mnt, struct mount, mnt);
}

static __always_inline bool prepend_path(struct path *path, bufs_t *string_p) {
  char slash = '/';
  char null = '\0';
  int offset = MAX_STRING_SIZE;

  if (path == NULL || string_p == NULL) {
    return false;
  }

  struct dentry *dentry = path->dentry;
  struct vfsmount *vfsmnt = path->mnt;

  struct mount *mnt = real_mount(vfsmnt);

  struct dentry *parent;
  struct dentry *mnt_root;
  struct mount *m;
  struct qstr d_name;

#pragma unroll
  for (int i = 0; i < 30; i++) {
    parent = BPF_CORE_READ(dentry, d_parent);
    mnt_root = BPF_CORE_READ(vfsmnt, mnt_root);

    if (dentry == mnt_root) {
      m = BPF_CORE_READ(mnt, mnt_parent);
      if (mnt != m) {
        dentry = BPF_CORE_READ(mnt, mnt_mountpoint);
        mnt = m;
        continue;
      }
      break;
    }

    if (dentry == parent) {
      break;
    }

    // get d_name
    d_name = BPF_CORE_READ(dentry, d_name);

    offset -= (d_name.len + 1);
    if (offset < 0)
      break;

    int sz = bpf_probe_read_str(
        &(string_p->buf[(offset) & (MAX_STRING_SIZE - 1)]),
        (d_name.len + 1) & (MAX_STRING_SIZE - 1), d_name.name);
    if (sz > 1) {
      bpf_probe_read(
          &(string_p->buf[(offset + d_name.len) & (MAX_STRING_SIZE - 1)]), 1,
          &slash);
    } else {
      offset += (d_name.len + 1);
    }

    dentry = parent;
  }

  if (offset == MAX_STRING_SIZE) {
    return false;
  }

  bpf_probe_read(&(string_p->buf[MAX_STRING_SIZE - 1]), 1, &null);
  offset--;

  bpf_probe_read(&(string_p->buf[offset & (MAX_STRING_SIZE - 1)]), 1, &slash);
  set_buf_off(PATH_BUFFER, offset);
  return true;
}

static __always_inline u32 get_task_pid_ns_id(struct task_struct *task) {
  return BPF_CORE_READ(task, nsproxy, pid_ns_for_children, ns).inum;
}

static __always_inline u32 get_task_mnt_ns_id(struct task_struct *task) {
  return BPF_CORE_READ(task, nsproxy, mnt_ns, ns).inum;
}

static struct file *get_task_file(struct task_struct *task) {
  return BPF_CORE_READ(task, mm, exe_file);
}

static inline void get_outer_key(struct outer_key *pokey,
                                 struct task_struct *t) {
  pokey->pid_ns = get_task_pid_ns_id(t);
  pokey->mnt_ns = get_task_mnt_ns_id(t);
  if (pokey->pid_ns == PROC_PID_INIT_INO) {
    pokey->pid_ns = 0;
    pokey->mnt_ns = 0;
  }
}

static bool is_owner(struct file *file_p) {
  kuid_t owner = BPF_CORE_READ(file_p, f_inode, i_uid);
  unsigned int z = bpf_get_current_uid_gid();
  if (owner.val != z)
    return false;
  return true;
}

static bool is_owner_path(struct dentry *dent) {
  kuid_t owner = BPF_CORE_READ(dent, d_inode, i_uid);
  unsigned int z = bpf_get_current_uid_gid();
  if (owner.val != z)
    return false;
  return true;
}

static inline int match_and_enforce_path_hooks(struct path *f_path, u32 id) {
  struct task_struct *t = (struct task_struct *)bpf_get_current_task();

  bool match = false;

  struct outer_key okey;
  get_outer_key(&okey, t);        //获取外部键（outer_key）

  u32 *inner = bpf_map_lookup_elem(&kubearmor_containers, &okey);    //在kubearmor_containers映射中查找outer_key，获取内部映射的指针inner

  if (!inner) {
    return 0;
  }

  // "z" is a zero value map key which is used to reset values of other keys
  // which are inturn used and updated to lookup the Rule Map

  // "store" stores informaton needed to do a lookup to our Rule Map

  // "pk" is a map key which is used for all kinds of matching and lookups, We
  // needed a third key because we need to copy contents from store and keep
  // resetting the contents of this key so data in store needs to persist
  // “z”是零值映射键，用于重置其他键的值
   // 依次使用和更新它们来查找规则映射

   //“store”存储查找规则映射所需的信息

   // “pk”是一个映射键，用于各种匹配和查找，我们
   // 需要第三个密钥，因为我们需要从存储复制内容并保留
   // 重置此键的内容，以便存储中的数据需要保留

  u32 zero = 0;
  bufs_k *z = bpf_map_lookup_elem(&bufk, &zero);    //在bufk映射中查找对应的元素，获取相应的bufs_k结构体指针：z
  if (z == NULL)
    return 0;

  u32 one = 1;
  bufs_k *store = bpf_map_lookup_elem(&bufk, &one);      //同上
  if (store == NULL)
    return 0;

  // Reset value for store
  bpf_map_update_elem(&bufk, &one, z, BPF_ANY);         

  u32 two = 2;
  bufs_k *pk = bpf_map_lookup_elem(&bufk, &two);         //同上
  if (pk == NULL)
    return 0;

  /* Extract full path from file structure provided by LSM Hook */
  /* 从 LSM Hook 提供的文件结构中提取完整路径 */
  bufs_t *path_buf = get_buf(PATH_BUFFER);          //从传入的路径结构体中提取完整路径，并将其存储在path_buf
  if (path_buf == NULL)
    return 0;

  if (!prepend_path(f_path, path_buf))
    return 0;

  u32 *path_offset = get_buf_off(PATH_BUFFER);
  if (path_offset == NULL)
    return 0;

  void *path_ptr = &path_buf->buf[*path_offset];
  bpf_probe_read_str(store->path, MAX_STRING_SIZE, path_ptr);

  /* Extract full path of the source binary from the task structure */
  /* 从task结构中提取源二进制文件的完整路径 */
  struct file *file_p = get_task_file(t);
  if (file_p == NULL)
    return 0;
  bufs_t *src_buf = get_buf(PATH_BUFFER);        //存储在src_buf
  if (src_buf == NULL)
    return 0;
  struct path f_src = BPF_CORE_READ(file_p, f_path);
  if (!prepend_path(&f_src, src_buf))
    return 0;

  u32 *src_offset = get_buf_off(PATH_BUFFER);
  if (src_offset == NULL)
    return 0;

  void *ptr = &src_buf->buf[*src_offset];
  bpf_probe_read_str(store->source, MAX_STRING_SIZE, ptr);

  struct data_t *val = bpf_map_lookup_elem(inner, store);      //在inner映射中使用store作为键，查找相应的数据结构data_t

  if (val && (val->filemask & RULE_READ)) {          //如果找到且具有RULE_READ标志，则将match设置为true，并跳转到决策（decision）标签
    match = true;
    goto decision;
  }

  struct data_t *dirval;
  bool recursivebuthint = false;

#pragma unroll
  for (int i = 0; i < 64; i++) {      
    if (store->path[i] == '\0')
      break;

    if (store->path[i] == '/') {
      bpf_map_update_elem(&bufk, &two, z, BPF_ANY);

      match = false;

      bpf_probe_read_str(pk->path, i + 2, store->path);
      /* Check Subdir with From Source */
      bpf_probe_read_str(pk->source, MAX_STRING_SIZE, store->source);
      dirval = bpf_map_lookup_elem(inner, pk);
      if (dirval) {
        if ((dirval->filemask & RULE_DIR) && (dirval->filemask & RULE_READ)) {
          match = true;
          bpf_printk("dir match %s with recursive %d and hint %d ", pk,
                     (dirval->filemask & RULE_RECURSIVE),
                     (dirval->filemask & RULE_HINT));
          bpf_printk("and from source %s\n", pk->source);
          if ((dirval->filemask &
               RULE_RECURSIVE)) { /* true directory match and */
                                  /* not a hint suggests */
            /* there are no possibility of child dir */
            val = dirval;
            if (dirval->filemask & RULE_HINT) {
              recursivebuthint = true;
              continue;
            } else {
              goto decision;
            }
          } else {
            continue; /* We continue the loop to see if we have more nested
                       */
                      /* directories and set match to false */
          }
        }
      } else {
        break;
      }
    }
  }

  if (recursivebuthint) {
    match = true;
    goto decision;
  }
  if (match) {
    if (dirval) { /* to please the holy verifier */
      val = dirval;
      goto decision;
    }
  }

  bpf_map_update_elem(&bufk, &two, z, BPF_ANY);
  bpf_probe_read_str(pk->path, MAX_STRING_SIZE, store->path);

  val = bpf_map_lookup_elem(inner, pk);

  if (val && (val->filemask & RULE_READ)) {
    match = true;
    goto decision;
  }

  recursivebuthint = false;

#pragma unroll
  for (int i = 0; i < 64; i++) {
    if (store->path[i] == '\0')
      break;

    if (store->path[i] == '/') {
      bpf_map_update_elem(&bufk, &two, z, BPF_ANY);
      match = false;
      bpf_probe_read_str(pk->path, i + 2, store->path);
      dirval = bpf_map_lookup_elem(inner, pk);
      if (!dirval) {
        break;
      }
      if ((dirval->filemask & RULE_DIR) && (dirval->filemask & RULE_READ)) {
        match = true;
        bpf_printk("dir match %s with recursive %d and hint %d ", pk,
                   (dirval->filemask & RULE_RECURSIVE),
                   (dirval->filemask & RULE_HINT));
        if (!(dirval->filemask & RULE_RECURSIVE)) {
          continue; /* continue the loop to see if we have more nested dirs */
        }
        /* true directory match and not a hint suggests */
        /* there are no possibility of child dir */
        val = dirval;
        if (dirval->filemask & RULE_HINT) {
          recursivebuthint = true;
          continue;
        }
        goto decision;
      }
    }
  }

  if (recursivebuthint) {
    match = true;
    goto decision;
  } else if (match && dirval) {
    val = dirval;
    goto decision;
  }

decision:

  if (id == dpath) { // Path Hooks
    if (match) {
      if (val && (val->filemask & RULE_OWNER)) {
        if (!is_owner_path(f_path->dentry)) {
          bpf_printk("denying path %s due to not owner\n", store);
          return -EPERM;
        }
        bpf_printk("allowing path %s for owner\n", store);
        return 0;
      }
      if (val && (val->filemask & RULE_DENY)) {
        bpf_printk("denying path %s due to in blacklist\n", store->path);
        return -EPERM;
      }
    }

    bpf_map_update_elem(&bufk, &two, z, BPF_ANY);
    pk->path[0] = dfile;
    struct data_t *allow = bpf_map_lookup_elem(inner, pk);

    if (allow) {
      if (!match) {
        bpf_printk("denying path %s due to not in allowlist, source -> %s\n",
                   store->path, store->source);
        return -EPERM;
      }
    }

  } else if (id == dfileread) { // file open
    if (match) {
      if (val && (val->filemask & RULE_OWNER)) {
        if (!is_owner_path(f_path->dentry)) {
          bpf_printk("denying file %s due to not owner\n", store);
          return -EPERM;
        }
        bpf_printk("allowing file %s for owner\n", store);
        return 0;
      }
      if (val && (val->filemask & RULE_READ) && !(val->filemask & RULE_WRITE)) {
        // Read Only Policy, Decision making will be done in lsm/file_permission
        return 0;
      }
      if (val && (val->filemask & RULE_DENY)) {
        bpf_printk("denying file %s due to in blacklist\n", store->path);
        return -EPERM;
      }
    }

    bpf_map_update_elem(&bufk, &two, z, BPF_ANY);
    pk->path[0] = dfile;
    struct data_t *allow = bpf_map_lookup_elem(inner, pk);

    if (allow && !match) {
      bpf_printk("denying file %s due to not in allowlist, source -> %s\n",
                 store->path, store->source);
      return -EPERM;
    }
  } else if (id == dfilewrite) { // fule write
    if (match) {
      if (val && (val->filemask & RULE_DENY)) {
        bpf_printk("denying file %s due to in blacklist\n", store->path);
        return -EPERM;
      }
    }

    bpf_map_update_elem(&bufk, &two, z, BPF_ANY);
    pk->path[0] = dfile;
    struct data_t *allow = bpf_map_lookup_elem(inner, pk);

    if (allow && !match) {
      bpf_printk("denying file %s due to not in allowlist, source -> %s\n",
                 store->path, store->source);
      return -EPERM;
    }
  }

  return 0;
}

/*
  How do we check what to deny or not?

  We match in the the following order:
  - entity + source
  -? directory matching + source
  - entity
  -? directory

  Once matched
  -? Owner Check
  - Deny Check
  - Check if WhiteList i.e. DefaultPosture for entity is block
  - if not match deny

  ? => Indicates optional check, like network hooks don't have owner or
       directory checks
*/

#endif /* __SHARED_H */
