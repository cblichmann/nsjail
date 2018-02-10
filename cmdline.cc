/*

   nsjail - cmdline parsing

   -----------------------------------------

   Copyright 2014 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include "cmdline.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>

#include "caps.h"
#include "config.h"
#include "log.h"
#include "macros.h"
#include "mnt.h"
#include "sandbox.h"
#include "user.h"
#include "util.h"

namespace cmdline {

struct custom_option {
	struct option opt;
	const char* descr;
};

// clang-format off
struct custom_option custom_opts[] = {
    { { "help", no_argument, NULL, 'h' }, "Help plz.." },
    { { "mode", required_argument, NULL, 'M' },
        "Execution mode (default: 'o' [MODE_STANDALONE_ONCE]):\n"
        "\tl: Wait for connections on a TCP port (specified with --port) [MODE_LISTEN_TCP]\n"
        "\to: Launch a single process on the console using clone/execve [MODE_STANDALONE_ONCE]\n"
        "\te: Launch a single process on the console using execve [MODE_STANDALONE_EXECVE]\n"
        "\tr: Launch a single process on the console with clone/execve, keep doing it forever [MODE_STANDALONE_RERUN]" },
    { { "config", required_argument, NULL, 'C' }, "Configuration file in the config.proto ProtoBuf format (see configs/ directory for examples)" },
    { { "exec_file", required_argument, NULL, 'x' }, "File to exec (default: argv[0])" },
    { { "execute_fd", no_argument, NULL, 0x0607 }, "Use execveat() to execute a file-descriptor instead of executing the binary path. In such case argv[0]/exec_file denotes a file path before mount namespacing" },
    { { "chroot", required_argument, NULL, 'c' }, "Directory containing / of the jail (default: none)" },
    { { "rw", no_argument, NULL, 0x601 }, "Mount chroot dir (/) R/W (default: R/O)" },
    { { "user", required_argument, NULL, 'u' }, "Username/uid of processess inside the jail (default: your current uid). You can also use inside_ns_uid:outside_ns_uid:count convention here. Can be specified multiple times" },
    { { "group", required_argument, NULL, 'g' }, "Groupname/gid of processess inside the jail (default: your current gid). You can also use inside_ns_gid:global_ns_gid:count convention here. Can be specified multiple times" },
    { { "hostname", required_argument, NULL, 'H' }, "UTS name (hostname) of the jail (default: 'NSJAIL')" },
    { { "cwd", required_argument, NULL, 'D' }, "Directory in the namespace the process will run (default: '/')" },
    { { "port", required_argument, NULL, 'p' }, "TCP port to bind to (enables MODE_LISTEN_TCP) (default: 0)" },
    { { "bindhost", required_argument, NULL, 0x604 }, "IP address to bind the port to (only in [MODE_LISTEN_TCP]), (default: '::')" },
    { { "max_conns_per_ip", required_argument, NULL, 'i' }, "Maximum number of connections per one IP (only in [MODE_LISTEN_TCP]), (default: 0 (unlimited))" },
    { { "log", required_argument, NULL, 'l' }, "Log file (default: use log_fd)" },
    { { "log_fd", required_argument, NULL, 'L' }, "Log FD (default: 2)" },
    { { "time_limit", required_argument, NULL, 't' }, "Maximum time that a jail can exist, in seconds (default: 600)" },
    { { "max_cpus", required_argument, NULL, 0x508 }, "Maximum number of CPUs a single jailed process can use (default: 0 'no limit')" },
    { { "daemon", no_argument, NULL, 'd' }, "Daemonize after start" },
    { { "verbose", no_argument, NULL, 'v' }, "Verbose output" },
    { { "quiet", no_argument, NULL, 'q' }, "Log warning and more important messages only" },
    { { "really_quiet", no_argument, NULL, 'Q' }, "Log fatal messages only" },
    { { "keep_env", no_argument, NULL, 'e' }, "Pass all environment variables to the child process (default: all envvars are cleared)" },
    { { "env", required_argument, NULL, 'E' }, "Additional environment variable (can be used multiple times)" },
    { { "keep_caps", no_argument, NULL, 0x0501 }, "Don't drop any capabilities" },
    { { "cap", required_argument, NULL, 0x0509 }, "Retain this capability, e.g. CAP_PTRACE (can be specified multiple times)" },
    { { "silent", no_argument, NULL, 0x0502 }, "Redirect child process' fd:0/1/2 to /dev/null" },
    { { "skip_setsid", no_argument, NULL, 0x0504 }, "Don't call setsid(), allows for terminal signal handling in the sandboxed process. Dangerous" },
    { { "pass_fd", required_argument, NULL, 0x0505 }, "Don't close this FD before executing the child process (can be specified multiple times), by default: 0/1/2 are kept open" },
    { { "disable_no_new_privs", no_argument, NULL, 0x0507 }, "Don't set the prctl(NO_NEW_PRIVS, 1) (DANGEROUS)" },
    { { "rlimit_as", required_argument, NULL, 0x0201 }, "RLIMIT_AS in MB, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 512)" },
    { { "rlimit_core", required_argument, NULL, 0x0202 }, "RLIMIT_CORE in MB, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 0)" },
    { { "rlimit_cpu", required_argument, NULL, 0x0203 }, "RLIMIT_CPU, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 600)" },
    { { "rlimit_fsize", required_argument, NULL, 0x0204 }, "RLIMIT_FSIZE in MB, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 1)" },
    { { "rlimit_nofile", required_argument, NULL, 0x0205 }, "RLIMIT_NOFILE, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 32)" },
    { { "rlimit_nproc", required_argument, NULL, 0x0206 }, "RLIMIT_NPROC, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 'soft')" },
    { { "rlimit_stack", required_argument, NULL, 0x0207 }, "RLIMIT_STACK in MB, 'max' or 'hard' for the current hard limit, 'def' or 'soft' for the current soft limit, 'inf' for RLIM64_INFINITY (default: 'soft')" },
    { { "persona_addr_compat_layout", no_argument, NULL, 0x0301 }, "personality(ADDR_COMPAT_LAYOUT)" },
    { { "persona_mmap_page_zero", no_argument, NULL, 0x0302 }, "personality(MMAP_PAGE_ZERO)" },
    { { "persona_read_implies_exec", no_argument, NULL, 0x0303 }, "personality(READ_IMPLIES_EXEC)" },
    { { "persona_addr_limit_3gb", no_argument, NULL, 0x0304 }, "personality(ADDR_LIMIT_3GB)" },
    { { "persona_addr_no_randomize", no_argument, NULL, 0x0305 }, "personality(ADDR_NO_RANDOMIZE)" },
    { { "disable_clone_newnet", no_argument, NULL, 'N' }, "Don't use CLONE_NEWNET. Enable global networking inside the jail" },
    { { "disable_clone_newuser", no_argument, NULL, 0x0402 }, "Don't use CLONE_NEWUSER. Requires euid==0" },
    { { "disable_clone_newns", no_argument, NULL, 0x0403 }, "Don't use CLONE_NEWNS" },
    { { "disable_clone_newpid", no_argument, NULL, 0x0404 }, "Don't use CLONE_NEWPID" },
    { { "disable_clone_newipc", no_argument, NULL, 0x0405 }, "Don't use CLONE_NEWIPC" },
    { { "disable_clone_newuts", no_argument, NULL, 0x0406 }, "Don't use CLONE_NEWUTS" },
    { { "disable_clone_newcgroup", no_argument, NULL, 0x0407 }, "Don't use CLONE_NEWCGROUP. Might be required for kernel versions < 4.6" },
    { { "uid_mapping", required_argument, NULL, 'U' }, "Add a custom uid mapping of the form inside_uid:outside_uid:count. Setting this requires newuidmap (set-uid) to be present" },
    { { "gid_mapping", required_argument, NULL, 'G' }, "Add a custom gid mapping of the form inside_gid:outside_gid:count. Setting this requires newgidmap (set-uid) to be present" },
    { { "bindmount_ro", required_argument, NULL, 'R' }, "List of mountpoints to be mounted --bind (ro) inside the container. Can be specified multiple times. Supports 'source' syntax, or 'source:dest'" },
    { { "bindmount", required_argument, NULL, 'B' }, "List of mountpoints to be mounted --bind (rw) inside the container. Can be specified multiple times. Supports 'source' syntax, or 'source:dest'" },
    { { "tmpfsmount", required_argument, NULL, 'T' }, "List of mountpoints to be mounted as tmpfs (R/W) inside the container. Can be specified multiple times. Supports 'dest' syntax" },
    { { "tmpfs_size", required_argument, NULL, 0x0602 }, "Number of bytes to allocate for tmpfsmounts (default: 4194304)" },
    { { "disable_proc", no_argument, NULL, 0x0603 }, "Disable mounting procfs in the jail" },
    { { "proc_path", required_argument, NULL, 0x0605 }, "Path used to mount procfs (default: '/proc')" },
    { { "proc_rw", no_argument, NULL, 0x0606 }, "Is procfs mounted as R/W (default: R/O)" },
    { { "seccomp_policy", required_argument, NULL, 'P' }, "Path to file containing seccomp-bpf policy (see kafel/)" },
    { { "seccomp_string", required_argument, NULL, 0x0901 }, "String with kafel seccomp-bpf policy (see kafel/)" },
    { { "cgroup_mem_max", required_argument, NULL, 0x0801 }, "Maximum number of bytes to use in the group (default: '0' - disabled)" },
    { { "cgroup_mem_mount", required_argument, NULL, 0x0802 }, "Location of memory cgroup FS (default: '/sys/fs/cgroup/memory')" },
    { { "cgroup_mem_parent", required_argument, NULL, 0x0803 }, "Which pre-existing memory cgroup to use as a parent (default: 'NSJAIL')" },
    { { "cgroup_pids_max", required_argument, NULL, 0x0811 }, "Maximum number of pids in a cgroup (default: '0' - disabled)" },
    { { "cgroup_pids_mount", required_argument, NULL, 0x0812 }, "Location of pids cgroup FS (default: '/sys/fs/cgroup/pids')" },
    { { "cgroup_pids_parent", required_argument, NULL, 0x0813 }, "Which pre-existing pids cgroup to use as a parent (default: 'NSJAIL')" },
    { { "cgroup_net_cls_classid", required_argument, NULL, 0x0821 }, "Class identifier of network packets in the group (default: '0' - disabled)" },
    { { "cgroup_net_cls_mount", required_argument, NULL, 0x0822 }, "Location of net_cls cgroup FS (default: '/sys/fs/cgroup/net_cls')" },
    { { "cgroup_net_cls_parent", required_argument, NULL, 0x0823 }, "Which pre-existing net_cls cgroup to use as a parent (default: 'NSJAIL')" },
    { { "cgroup_cpu_ms_per_sec", required_argument, NULL, 0x0831 }, "Number of us that the process group can use per second (default: '0' - disabled)" },
    { { "cgroup_cpu_mount", required_argument, NULL, 0x0822 }, "Location of cpu cgroup FS (default: '/sys/fs/cgroup/net_cls')" },
    { { "cgroup_cpu_parent", required_argument, NULL, 0x0833 }, "Which pre-existing cpu cgroup to use as a parent (default: 'NSJAIL')" },
    { { "iface_no_lo", no_argument, NULL, 0x700 }, "Don't bring the 'lo' interface up" },
    { { "macvlan_iface", required_argument, NULL, 'I' }, "Interface which will be cloned (MACVLAN) and put inside the subprocess' namespace as 'vs'" },
    { { "macvlan_vs_ip", required_argument, NULL, 0x701 }, "IP of the 'vs' interface (e.g. \"192.168.0.1\")" },
    { { "macvlan_vs_nm", required_argument, NULL, 0x702 }, "Netmask of the 'vs' interface (e.g. \"255.255.255.0\")" },
    { { "macvlan_vs_gw", required_argument, NULL, 0x703 }, "Default GW for the 'vs' interface (e.g. \"192.168.0.1\")" },
};

struct custom_option deprecated_opts[] = {
    // Compatibilty flags for MACVLAN.
    // TODO(rswiecki): Remove this at some point.
    { { "iface", required_argument, NULL, 'I' }, "Interface which will be cloned (MACVLAN) and put inside the subprocess' namespace as 'vs'" },
    { { "iface_vs_ip", required_argument, NULL, 0x701 }, "IP of the 'vs' interface (e.g. \"192.168.0.1\")" },
    { { "iface_vs_nm", required_argument, NULL, 0x702 }, "Netmask of the 'vs' interface (e.g. \"255.255.255.0\")" },
    { { "iface_vs_gw", required_argument, NULL, 0x703 }, "Default GW for the 'vs' interface (e.g. \"192.168.0.1\")" },
    { { "enable_clone_newcgroup", no_argument, NULL, 0x0408 }, "Use CLONE_NEWCGROUP (it's enabled by default now)" },
};
// clang-format on

static const char* logYesNo(bool yes) { return (yes ? "true" : "false"); }

static void cmdlineOptUsage(struct custom_option* option) {
	if (option->opt.val < 0x80) {
		LOG_HELP_BOLD(" --%s%s%c %s", option->opt.name, "|-", option->opt.val,
		    option->opt.has_arg == required_argument ? "VALUE" : "");
	} else {
		LOG_HELP_BOLD(" --%s %s", option->opt.name,
		    option->opt.has_arg == required_argument ? "VALUE" : "");
	}
	LOG_HELP("\t%s", option->descr);
}

static void cmdlineUsage(const char* pname) {
	LOG_HELP_BOLD("Usage: %s [options] -- path_to_command [args]", pname);
	LOG_HELP_BOLD("Options:");
	for (size_t i = 0; i < ARRAYSIZE(custom_opts); i++) {
		cmdlineOptUsage(&custom_opts[i]);
	}
	LOG_HELP_BOLD("\nDeprecated options:");
	for (size_t i = 0; i < ARRAYSIZE(deprecated_opts); i++) {
		cmdlineOptUsage(&deprecated_opts[i]);
		// Find replacement flag.
		for (size_t j = 0; j < ARRAYSIZE(custom_opts); j++) {
			if (custom_opts[j].opt.val == deprecated_opts[i].opt.val) {
				LOG_HELP_BOLD(
				    "\tDEPRECATED: Use %s instead.", custom_opts[j].opt.name);
				break;
			}
		}
	}
	LOG_HELP_BOLD("\n Examples: ");
	LOG_HELP(" Wait on a port 31337 for connections, and run /bin/sh");
	LOG_HELP_BOLD("  nsjail -Ml --port 31337 --chroot / -- /bin/sh -i");
	LOG_HELP(" Re-run echo command as a sub-process");
	LOG_HELP_BOLD("  nsjail -Mr --chroot / -- /bin/echo \"ABC\"");
	LOG_HELP(" Run echo command once only, as a sub-process");
	LOG_HELP_BOLD("  nsjail -Mo --chroot / -- /bin/echo \"ABC\"");
	LOG_HELP(" Execute echo command directly, without a supervising process");
	LOG_HELP_BOLD("  nsjail -Me --chroot / --disable_proc -- /bin/echo \"ABC\"");
}

void logParams(struct nsjconf_t* nsjconf) {
	switch (nsjconf->mode) {
	case MODE_LISTEN_TCP:
		LOG_I("Mode: LISTEN_TCP");
		break;
	case MODE_STANDALONE_ONCE:
		LOG_I("Mode: STANDALONE_ONCE");
		break;
	case MODE_STANDALONE_EXECVE:
		LOG_I("Mode: STANDALONE_EXECVE");
		break;
	case MODE_STANDALONE_RERUN:
		LOG_I("Mode: STANDALONE_RERUN");
		break;
	default:
		LOG_F("Mode: UNKNOWN");
		break;
	}

	LOG_I(
	    "Jail parameters: hostname:'%s', chroot:'%s', process:'%s', "
	    "bind:[%s]:%d, "
	    "max_conns_per_ip:%u, time_limit:%ld, personality:%#lx, daemonize:%s, "
	    "clone_newnet:%s, clone_newuser:%s, clone_newns:%s, clone_newpid:%s, "
	    "clone_newipc:%s, clonew_newuts:%s, clone_newcgroup:%s, keep_caps:%s, "
	    "tmpfs_size:%zu, disable_no_new_privs:%s, max_cpus:%zu",
	    nsjconf->hostname.c_str(), nsjconf->chroot.c_str(), nsjconf->argv[0], nsjconf->bindhost,
	    nsjconf->port, nsjconf->max_conns_per_ip, nsjconf->tlimit, nsjconf->personality,
	    logYesNo(nsjconf->daemonize), logYesNo(nsjconf->clone_newnet),
	    logYesNo(nsjconf->clone_newuser), logYesNo(nsjconf->clone_newns),
	    logYesNo(nsjconf->clone_newpid), logYesNo(nsjconf->clone_newipc),
	    logYesNo(nsjconf->clone_newuts), logYesNo(nsjconf->clone_newcgroup),
	    logYesNo(nsjconf->keep_caps), nsjconf->tmpfs_size,
	    logYesNo(nsjconf->disable_no_new_privs), nsjconf->max_cpus);

	{
		struct mounts_t* p;
		TAILQ_FOREACH(p, &nsjconf->mountpts, pointers) {
			LOG_I("%s: %s", p->isSymlink ? "Symlink" : "Mount point",
			    mnt::describeMountPt(p));
		}
	}
	{
		struct idmap_t* p;
		for (const auto& uid : nsjconf->uids) {
			LOG_I("Uid map: inside_uid:%lu outside_uid:%lu count:%zu newuidmap:%s",
			    (unsigned long)uid.inside_id, (unsigned long)uid.outside_id, uid.count,
			    uid.is_newidmap ? "true" : "false");
			if (uid.outside_id == 0 && nsjconf->clone_newuser) {
				LOG_W(
				    "Process will be UID/EUID=0 in the global user namespace, "
				    "and will have user "
				    "root-level access to files");
			}
		}
		for (const auto& gid : nsjconf->gids) {
			LOG_I("Gid map: inside_gid:%lu outside_gid:%lu count:%zu newgidmap:%s",
			    (unsigned long)gid.inside_id, (unsigned long)gid.outside_id, gid.count,
			    gid.is_newidmap ? "true" : "false");
			if (gid.outside_id == 0 && nsjconf->clone_newuser) {
				LOG_W(
				    "Process will be GID/EGID=0 in the global user namespace, "
				    "and will have group "
				    "root-level access to files");
			}
		}
	}
}

uint64_t parseRLimit(int res, const char* optarg, unsigned long mul) {
	if (strcasecmp(optarg, "inf") == 0) {
		return RLIM64_INFINITY;
	}
	struct rlimit64 cur;
	if (getrlimit64(res, &cur) == -1) {
		PLOG_F("getrlimit(%d)", res);
	}
	if (strcasecmp(optarg, "def") == 0 || strcasecmp(optarg, "soft") == 0) {
		return cur.rlim_cur;
	}
	if (strcasecmp(optarg, "max") == 0 || strcasecmp(optarg, "hard") == 0) {
		return cur.rlim_max;
	}
	if (util::isANumber(optarg) == false) {
		LOG_F(
		    "RLIMIT %d needs a numeric or 'max'/'hard'/'def'/'soft'/'inf' value ('%s' "
		    "provided)",
		    res, optarg);
	}
	uint64_t val = strtoull(optarg, NULL, 0) * mul;
	if (val == ULLONG_MAX && errno != 0) {
		PLOG_F("strtoul('%s', 0)", optarg);
	}
	return val;
}

/* findSpecDestination mutates spec (source:dest) to have a null byte instead
 * of ':' in between source and dest, then returns a pointer to the dest
 * string. */
static char* cmdlineSplitStrByColon(char* spec) {
	if (spec == NULL) {
		return NULL;
	}

	char* dest = spec;
	while (*dest != ':' && *dest != '\0') {
		dest++;
	}

	switch (*dest) {
	case ':':
		*dest = '\0';
		return dest + 1;
	case '\0':
		return NULL;
	default:
		LOG_F("Impossible condition in cmdlineSplitStrByColon()");
		return NULL;
	}
}

std::unique_ptr<struct nsjconf_t> parseArgs(int argc, char* argv[]) {
	std::unique_ptr<struct nsjconf_t> nsjconf = std::make_unique<struct nsjconf_t>();

	nsjconf->exec_file = NULL;
	nsjconf->use_execveat = false;
	nsjconf->exec_fd = -1;
	nsjconf->argv = NULL;
	nsjconf->hostname = "NSJAIL";
	nsjconf->cwd = "/";
	nsjconf->port = 0;
	nsjconf->bindhost = "::";
	nsjconf->log_fd = STDERR_FILENO;
	nsjconf->loglevel = INFO;
	nsjconf->daemonize = false;
	nsjconf->tlimit = 0;
	nsjconf->max_cpus = 0;
	nsjconf->keep_caps = false;
	nsjconf->disable_no_new_privs = false;
	nsjconf->rl_as = 512 * (1024 * 1024);
	nsjconf->rl_core = 0;
	nsjconf->rl_cpu = 600;
	nsjconf->rl_fsize = 1 * (1024 * 1024);
	nsjconf->rl_nofile = 32;
	nsjconf->rl_nproc = parseRLimit(RLIMIT_NPROC, "soft", 1);
	nsjconf->rl_stack = parseRLimit(RLIMIT_STACK, "soft", 1);
	nsjconf->personality = 0;
	nsjconf->clone_newnet = true;
	nsjconf->clone_newuser = true;
	nsjconf->clone_newns = true;
	nsjconf->clone_newpid = true;
	nsjconf->clone_newipc = true;
	nsjconf->clone_newuts = true;
	nsjconf->clone_newcgroup = true;
	nsjconf->mode = MODE_STANDALONE_ONCE;
	nsjconf->is_root_rw = false;
	nsjconf->is_silent = false;
	nsjconf->skip_setsid = false;
	nsjconf->max_conns_per_ip = 0;
	nsjconf->tmpfs_size = 4 * (1024 * 1024);
	nsjconf->mount_proc = true;
	nsjconf->proc_path = "/proc";
	nsjconf->is_proc_rw = false;
	nsjconf->cgroup_mem_mount = "/sys/fs/cgroup/memory";
	nsjconf->cgroup_mem_parent = "NSJAIL";
	nsjconf->cgroup_mem_max = (size_t)0;
	nsjconf->cgroup_pids_mount = "/sys/fs/cgroup/pids";
	nsjconf->cgroup_pids_parent = "NSJAIL";
	nsjconf->cgroup_pids_max = 0U;
	nsjconf->cgroup_net_cls_mount = "/sys/fs/cgroup/net_cls";
	nsjconf->cgroup_net_cls_parent = "NSJAIL";
	nsjconf->cgroup_net_cls_classid = 0U;
	nsjconf->cgroup_cpu_mount = "/sys/fs/cgroup/cpu";
	nsjconf->cgroup_cpu_parent = "NSJAIL";
	nsjconf->cgroup_cpu_ms_per_sec = 0U;
	nsjconf->iface_no_lo = false;
	nsjconf->iface_vs = NULL;
	nsjconf->iface_vs_ip = "0.0.0.0";
	nsjconf->iface_vs_nm = "255.255.255.0";
	nsjconf->iface_vs_gw = "0.0.0.0";
	nsjconf->kafel_file_path = NULL;
	nsjconf->kafel_string = NULL;
	nsjconf->orig_uid = getuid();
	nsjconf->num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	nsjconf->openfds.push_back(STDIN_FILENO);
	nsjconf->openfds.push_back(STDOUT_FILENO);
	nsjconf->openfds.push_back(STDERR_FILENO);

	TAILQ_INIT(&nsjconf->mountpts);

	static char cmdlineTmpfsSz[PATH_MAX] = "size=4194304";

	// Generate options array for getopt_long.
	size_t options_length = ARRAYSIZE(custom_opts) + ARRAYSIZE(deprecated_opts) + 1;
	struct option opts[options_length];
	for (unsigned i = 0; i < ARRAYSIZE(custom_opts); i++) {
		opts[i] = custom_opts[i].opt;
	}
	for (unsigned i = 0; i < ARRAYSIZE(deprecated_opts); i++) {
		opts[ARRAYSIZE(custom_opts) + i] = deprecated_opts[i].opt;
	}
	// Last, NULL option as a terminator.
	struct option terminator = {NULL, 0, NULL, 0};
	memcpy(&opts[options_length - 1].name, &terminator, sizeof(terminator));

	int opt_index = 0;
	for (;;) {
		int c = getopt_long(argc, argv,
		    "x:H:D:C:c:p:i:u:g:l:L:t:M:NdvqQeh?E:R:B:T:P:I:U:G:", opts, &opt_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'x':
			nsjconf->exec_file = optarg;
			break;
		case 'H':
			nsjconf->hostname = optarg;
			break;
		case 'D':
			nsjconf->cwd = optarg;
			break;
		case 'C':
			if (config::parseFile(nsjconf.get(), optarg) == false) {
				LOG_F("Couldn't parse configuration from '%s' file", optarg);
			}
			break;
		case 'c':
			nsjconf->chroot = optarg;
			break;
		case 'p':
			nsjconf->port = strtoul(optarg, NULL, 0);
			nsjconf->mode = MODE_LISTEN_TCP;
			break;
		case 0x604:
			nsjconf->bindhost = optarg;
			break;
		case 'i':
			nsjconf->max_conns_per_ip = strtoul(optarg, NULL, 0);
			break;
		case 'l':
			nsjconf->logfile = optarg;
			if (!log::initLogFile(nsjconf.get())) {
				return nullptr;
			}
			break;
		case 'L':
			nsjconf->log_fd = strtol(optarg, NULL, 0);
			if (log::initLogFile(nsjconf.get()) == false) {
				return nullptr;
			}
			break;
		case 'd':
			nsjconf->daemonize = true;
			break;
		case 'v':
			nsjconf->loglevel = DEBUG;
			if (log::initLogFile(nsjconf.get()) == false) {
				return nullptr;
			}
			break;
		case 'q':
			nsjconf->loglevel = WARNING;
			if (log::initLogFile(nsjconf.get()) == false) {
				return nullptr;
			}
			break;
		case 'Q':
			nsjconf->loglevel = FATAL;
			if (log::initLogFile(nsjconf.get()) == false) {
				return nullptr;
			}
			break;
		case 'e':
			nsjconf->keep_env = true;
			break;
		case 't':
			nsjconf->tlimit = strtol(optarg, NULL, 0);
			break;
		case 'h': /* help */
			cmdlineUsage(argv[0]);
			exit(0);
			break;
		case 0x0201:
			nsjconf->rl_as = parseRLimit(RLIMIT_AS, optarg, (1024 * 1024));
			break;
		case 0x0202:
			nsjconf->rl_core = parseRLimit(RLIMIT_CORE, optarg, (1024 * 1024));
			break;
		case 0x0203:
			nsjconf->rl_cpu = parseRLimit(RLIMIT_CPU, optarg, 1);
			break;
		case 0x0204:
			nsjconf->rl_fsize = parseRLimit(RLIMIT_FSIZE, optarg, (1024 * 1024));
			break;
		case 0x0205:
			nsjconf->rl_nofile = parseRLimit(RLIMIT_NOFILE, optarg, 1);
			break;
		case 0x0206:
			nsjconf->rl_nproc = parseRLimit(RLIMIT_NPROC, optarg, 1);
			break;
		case 0x0207:
			nsjconf->rl_stack = parseRLimit(RLIMIT_STACK, optarg, (1024 * 1024));
			break;
		case 0x0301:
			nsjconf->personality |= ADDR_COMPAT_LAYOUT;
			break;
		case 0x0302:
			nsjconf->personality |= MMAP_PAGE_ZERO;
			break;
		case 0x0303:
			nsjconf->personality |= READ_IMPLIES_EXEC;
			break;
		case 0x0304:
			nsjconf->personality |= ADDR_LIMIT_3GB;
			break;
		case 0x0305:
			nsjconf->personality |= ADDR_NO_RANDOMIZE;
			break;
		case 'N':
			nsjconf->clone_newnet = false;
			break;
		case 0x0402:
			nsjconf->clone_newuser = false;
			break;
		case 0x0403:
			nsjconf->clone_newns = false;
			break;
		case 0x0404:
			nsjconf->clone_newpid = false;
			break;
		case 0x0405:
			nsjconf->clone_newipc = false;
			break;
		case 0x0406:
			nsjconf->clone_newuts = false;
			break;
		case 0x0407:
			nsjconf->clone_newcgroup = false;
			break;
		case 0x0408:
			nsjconf->clone_newcgroup = true;
			break;
		case 0x0501:
			nsjconf->keep_caps = true;
			break;
		case 0x0502:
			nsjconf->is_silent = true;
			break;
		case 0x0504:
			nsjconf->skip_setsid = true;
			break;
		case 0x0505:
			nsjconf->openfds.push_back((int)strtol(optarg, NULL, 0));
			break;
		case 0x0507:
			nsjconf->disable_no_new_privs = true;
			break;
		case 0x0508:
			nsjconf->max_cpus = strtoul(optarg, NULL, 0);
			break;
		case 0x0509: {
			int cap = caps::nameToVal(optarg);
			if (cap == -1) {
				return nullptr;
			}
			nsjconf->caps.push_back(cap);
		} break;
		case 0x0601:
			nsjconf->is_root_rw = true;
			break;
		case 0x0602:
			nsjconf->tmpfs_size = strtoull(optarg, NULL, 0);
			snprintf(cmdlineTmpfsSz, sizeof(cmdlineTmpfsSz), "size=%zu",
			    nsjconf->tmpfs_size);
			break;
		case 0x0603:
			nsjconf->mount_proc = false;
			break;
		case 0x0605:
			nsjconf->proc_path = optarg;
			break;
		case 0x0606:
			nsjconf->is_proc_rw = true;
			break;
		case 0x0607:
			nsjconf->use_execveat = true;
			break;
		case 'E':
			nsjconf->envs.push_back(optarg);
			break;
		case 'u': {
			char* i_id = optarg;
			char* o_id = cmdlineSplitStrByColon(i_id);
			char* cnt = cmdlineSplitStrByColon(o_id);
			size_t count =
			    (cnt == NULL || strlen(cnt) == 0) ? 1U : (size_t)strtoull(cnt, NULL, 0);
			if (user::parseId(nsjconf.get(), i_id, o_id, count, false /* is_gid */,
				false /* is_newidmap */) == false) {
				return nullptr;
			}
		} break;
		case 'g': {
			char* i_id = optarg;
			char* o_id = cmdlineSplitStrByColon(i_id);
			char* cnt = cmdlineSplitStrByColon(o_id);
			size_t count =
			    (cnt == NULL || strlen(cnt) == 0) ? 1U : (size_t)strtoull(cnt, NULL, 0);
			if (user::parseId(nsjconf.get(), i_id, o_id, count, true /* is_gid */,
				false /* is_newidmap */) == false) {
				return nullptr;
			}
		} break;
		case 'U': {
			char* i_id = optarg;
			char* o_id = cmdlineSplitStrByColon(i_id);
			char* cnt = cmdlineSplitStrByColon(o_id);
			size_t count =
			    (cnt == NULL || strlen(cnt) == 0) ? 1U : (size_t)strtoull(cnt, NULL, 0);
			if (user::parseId(nsjconf.get(), i_id, o_id, count, false /* is_gid */,
				true /* is_newidmap */) == false) {
				return nullptr;
			}
		} break;
		case 'G': {
			char* i_id = optarg;
			char* o_id = cmdlineSplitStrByColon(i_id);
			char* cnt = cmdlineSplitStrByColon(o_id);
			size_t count =
			    (cnt == NULL || strlen(cnt) == 0) ? 1U : (size_t)strtoull(cnt, NULL, 0);
			if (user::parseId(nsjconf.get(), i_id, o_id, count, true /* is_gid */,
				true /* is_newidmap */) == false) {
				return nullptr;
			}
		} break;
		case 'R': {
			const char* dst = cmdlineSplitStrByColon(optarg);
			dst = dst ? dst : optarg;
			if (!mnt::addMountPtTail(nsjconf.get(), /* src= */ optarg, dst,
				/* fs_type= */ "",
				/* options= */ "", MS_BIND | MS_REC | MS_PRIVATE | MS_RDONLY,
				/* isDir= */ mnt::NS_DIR_MAYBE, /* mandatory= */ true, NULL, NULL,
				NULL, 0, /* is_symlink= */ false)) {
				return nullptr;
			}
		}; break;
		case 'B': {
			const char* dst = cmdlineSplitStrByColon(optarg);
			dst = dst ? dst : optarg;
			if (!mnt::addMountPtTail(nsjconf.get(), /* src= */ optarg, dst,
				/* fs_type= */ "",
				/* options= */ "", MS_BIND | MS_REC | MS_PRIVATE,
				/* isDir= */ mnt::NS_DIR_MAYBE, /* mandatory= */ true, NULL, NULL,
				NULL, 0, /* is_symlink= */ false)) {
				return nullptr;
			}
		}; break;
		case 'T': {
			if (!mnt::addMountPtTail(nsjconf.get(), /* src= */ NULL, optarg, "tmpfs",
				/* options= */ cmdlineTmpfsSz, /* flags= */ 0,
				/* isDir= */ mnt::NS_DIR_YES,
				/* mandatory= */ true, NULL, NULL, NULL, 0,
				/* is_symlink= */ false)) {
				return nullptr;
			}
		}; break;
		case 'M':
			switch (optarg[0]) {
			case 'l':
				nsjconf->mode = MODE_LISTEN_TCP;
				break;
			case 'o':
				nsjconf->mode = MODE_STANDALONE_ONCE;
				break;
			case 'e':
				nsjconf->mode = MODE_STANDALONE_EXECVE;
				break;
			case 'r':
				nsjconf->mode = MODE_STANDALONE_RERUN;
				break;
			default:
				LOG_E("Modes supported: -M l - MODE_LISTEN_TCP (default)");
				LOG_E("                 -M o - MODE_STANDALONE_ONCE");
				LOG_E("                 -M r - MODE_STANDALONE_RERUN");
				LOG_E("                 -M e - MODE_STANDALONE_EXECVE");
				cmdlineUsage(argv[0]);
				return nullptr;
				break;
			}
			break;
		case 0x700:
			nsjconf->iface_no_lo = true;
			break;
		case 'I':
			nsjconf->iface_vs = optarg;
			break;
		case 0x701:
			nsjconf->iface_vs_ip = optarg;
			break;
		case 0x702:
			nsjconf->iface_vs_nm = optarg;
			break;
		case 0x703:
			nsjconf->iface_vs_gw = optarg;
			break;
		case 0x801:
			nsjconf->cgroup_mem_max = (size_t)strtoull(optarg, NULL, 0);
			break;
		case 0x802:
			nsjconf->cgroup_mem_mount = optarg;
			break;
		case 0x803:
			nsjconf->cgroup_mem_parent = optarg;
			break;
		case 0x811:
			nsjconf->cgroup_pids_max = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 0x812:
			nsjconf->cgroup_pids_mount = optarg;
			break;
		case 0x813:
			nsjconf->cgroup_pids_parent = optarg;
			break;
		case 0x821:
			nsjconf->cgroup_net_cls_classid = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 0x822:
			nsjconf->cgroup_net_cls_mount = optarg;
			break;
		case 0x823:
			nsjconf->cgroup_net_cls_parent = optarg;
			break;
		case 0x831:
			nsjconf->cgroup_cpu_ms_per_sec = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 0x832:
			nsjconf->cgroup_cpu_mount = optarg;
			break;
		case 0x833:
			nsjconf->cgroup_cpu_parent = optarg;
			break;
		case 'P':
			nsjconf->kafel_file_path = optarg;
			if (access(nsjconf->kafel_file_path, R_OK) == -1) {
				PLOG_E("kafel config file '%s' cannot be opened for reading",
				    nsjconf->kafel_file_path);
				return nullptr;
			}
			break;
		case 0x0901:
			nsjconf->kafel_string = optarg;
			break;
		default:
			cmdlineUsage(argv[0]);
			return nullptr;
			break;
		}
	}

	if (nsjconf->mount_proc) {
		if (!mnt::addMountPtTail(nsjconf.get(), /* src= */ NULL, nsjconf->proc_path, "proc",
			"", nsjconf->is_proc_rw ? 0 : MS_RDONLY, /* isDir= */ mnt::NS_DIR_YES,
			/* mandatory= */ true, NULL, NULL, NULL, 0, /* is_symlink= */ false)) {
			return nullptr;
		}
	}
	if (!(nsjconf->chroot.empty())) {
		if (!mnt::addMountPtHead(nsjconf.get(), nsjconf->chroot.c_str(), "/",
			/* fs_type= */ "",
			/* options= */ "",
			nsjconf->is_root_rw ? (MS_BIND | MS_REC | MS_PRIVATE)
					    : (MS_BIND | MS_REC | MS_PRIVATE | MS_RDONLY),
			/* isDir= */ mnt::NS_DIR_YES, /* mandatory= */ true, NULL, NULL, NULL, 0,
			/* is_symlink= */ false)) {
			return nullptr;
		}
	} else {
		if (!mnt::addMountPtHead(nsjconf.get(), /* src= */ NULL, "/", "tmpfs",
			/* options= */ "", nsjconf->is_root_rw ? 0 : MS_RDONLY,
			/* isDir= */ mnt::NS_DIR_YES,
			/* mandatory= */ true, NULL, NULL, NULL, 0, /* is_symlink= */ false)) {
			return nullptr;
		}
	}

	if (nsjconf->uids.empty()) {
		struct idmap_t uid;
		uid.inside_id = getuid();
		uid.outside_id = getuid();
		uid.count = 1U;
		uid.is_newidmap = false;
		nsjconf->uids.push_back(uid);
	}
	if (nsjconf->gids.empty()) {
		struct idmap_t gid;
		gid.inside_id = getgid();
		gid.outside_id = getgid();
		gid.count = 1U;
		gid.is_newidmap = false;
		nsjconf->gids.push_back(gid);
	}

	if (log::initLogFile(nsjconf.get()) == false) {
		return nullptr;
	}

	if (argv[optind]) {
		nsjconf->argv = (const char**)&argv[optind];
	}
	if (nsjconf->argv == NULL || nsjconf->argv[0] == NULL) {
		cmdlineUsage(argv[0]);
		LOG_E("No command provided");
		return nullptr;
	}
	if (nsjconf->exec_file == NULL) {
		nsjconf->exec_file = nsjconf->argv[0];
	}

	if (nsjconf->use_execveat) {
#if !defined(__NR_execveat)
		LOG_E(
		    "Your nsjail is compiled without support for the execveat() syscall, yet you "
		    "specified the --execute_fd flag");
		return nullptr;
#endif /* !defined(__NR_execveat) */
		if ((nsjconf->exec_fd = open(nsjconf->exec_file, O_RDONLY | O_PATH | O_CLOEXEC)) ==
		    -1) {
			PLOG_W("Couldn't open '%s' file", nsjconf->exec_file);
			return nullptr;
		}
	}

	if (!sandbox::preparePolicy(nsjconf.get())) {
		LOG_E("Couldn't prepare sandboxing setup");
		return nullptr;
	}

	return nsjconf;
}

}  // namespace cmdline
