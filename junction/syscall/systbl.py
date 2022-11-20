#!/usr/bin/env python3

import sys
import os

assert len(sys.argv) == 4

SYSCALL_DEFS = sys.argv[1]
USYS_LIST = sys.argv[2]
OUTPUT_FILE = sys.argv[3]

SYS_NR = 451

def gen_syscall_dict():
	with open(SYSCALL_DEFS) as f:
		dat = f.read().splitlines()
	syscall_nr_to_name = {}
	syscall_name_to_nr = {}
	for line in dat:
		ls = line.strip().split("#define __NR_")
		if len(ls) > 1:
			name, nr = ls[1].split()
			syscall_nr_to_name[int(nr)] = name
			syscall_name_to_nr[name] = int(nr)
	return syscall_nr_to_name, syscall_name_to_nr

def gen_usys_list():
	with open(USYS_LIST) as f:
		for line in f:
			line = line.strip()
			if not line or line.startswith("#"):
				continue
			yield line

syscall_nr_to_name, syscall_name_to_nr = gen_syscall_dict()

filename = os.path.basename(OUTPUT_FILE)
dispatch_file = [f"// {filename} - Generated by systbl.py - do not modify", "", ""]
dispatch_file += ["#include \"junction/syscall/systbl.hpp\"", "#include \"junction/bindings/log.h\""]
dispatch_file += [f"static_assert(SYS_NR == {SYS_NR});"] # Make sure we are in sync with the header
dispatch_file += ["namespace junction {"]

defined_syscalls = [None for i in range(SYS_NR)]

for name in gen_usys_list():
	defined_syscalls[syscall_name_to_nr.get(name)] = f"junction::usys_{name}"

# generate stub functions for unimplemented syscalls
# TODO: eventually replace these with a single function
for i, entry in enumerate(defined_syscalls):
	if entry: continue
	name = syscall_nr_to_name.get(i, str(i)) 
	fn = f"""
long usys_{name}_fwd(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {'{'}
  LOG(WARN) << "Syscall {name} unimplemented, forwarding to kernel";
  return ksys_default({i}, arg0, arg1, arg2, arg3, arg4, arg5);
{'}'}"""
	dispatch_file.append(fn)

# generate the sysfn table
dispatch_file += [f"sysfn_t sys_tbl[SYS_NR] = {'{'}"]
for i, entry in enumerate(defined_syscalls):
	idx = f"SYS_{syscall_nr_to_name[i]}" if i in syscall_nr_to_name else i
	if not entry:
		name = syscall_nr_to_name.get(i, str(i))
		entry = f"usys_{name}_fwd"
	dispatch_file.append(f"\t[{idx}] = reinterpret_cast<sysfn_t>(&{entry}),")
dispatch_file.append("};")

# finish file and write it out
dispatch_file.append("}  // namespace junction")

with open(OUTPUT_FILE, "w") as f:
	f.write("\n".join(dispatch_file))

