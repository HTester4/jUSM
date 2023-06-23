// syscall.hpp - support for junction syscalls

#pragma once

#include <syscall.h>

#include "junction/kernel/ksys.h"
#include "junction/kernel/usys.h"

#define SYS_NR 453
#define SYSTBL_TRAMPOLINE_LOC ((void *)0x200000)

namespace junction {

extern "C" typedef uint64_t (*sysfn_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                                       uint64_t, uint64_t);

// generated by systbl.py
extern sysfn_t sys_tbl[SYS_NR];
extern sysfn_t sys_tbl_strace[SYS_NR];
extern const char *syscall_names[SYS_NR];
}  // namespace junction
