// SPDX-License-Identifier: GPL-2.0
/*
 * tier_ext_load: attach a tier_ext BPF policy to (cgroup, nodes).
 *
 *   tier_ext_load [-c CGROUP_DIR | -i CGROUP_ID] [-n NODELIST] [-s VAR=VAL]...
 *                 [-q] POLICY.bpf.o [-- COMMAND ARGS...]
 *
 * Opens POLICY.bpf.o, points every struct tier_ext_ops in it at the cgroup
 * and nodes, sets global variables given with -s, loads and attaches it. With
 * a COMMAND, the command runs while the policy is attached and its exit code
 * is returned; otherwise the policy stays attached until SIGINT/SIGTERM. On
 * exit the policy's global variables (counters) are printed, unless -q.
 *
 * Engine policies (te_engine.bpf.h) contain a "te_init" syscall program; it is
 * run once after loading with the machine's highest pfn, to allocate the
 * per-pfn node array. A global variable "max_pfn" is set the same way. A
 * "te_periodic" syscall program, if present, runs about once a second while
 * the policy is attached (e.g. lhd rebuilds its model there).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>

#define MAX_SETS 32

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	stop = 1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-c CGROUP_DIR | -i CGROUP_ID] [-n NODELIST] [-s VAR=VAL]... [-q]\n"
		"          POLICY.bpf.o [-- COMMAND ARGS...]\n"
		"  -c  cgroup v2 directory (default: root cgroup)\n"
		"  -i  cgroup id instead of a directory\n"
		"  -n  NUMA nodes, e.g. 0 or 0,2-3 (default: all)\n"
		"  -s  set a global variable of the policy before loading\n"
		"  -q  do not print the policy's variables on exit\n", prog);
	exit(2);
}

static int parse_nodes(const char *s, unsigned long long *mask)
{
	char *end;

	*mask = 0;
	while (*s) {
		long a = strtol(s, &end, 10), b = a;

		if (end == s)
			return -1;
		if (*end == '-') {
			s = end + 1;
			b = strtol(s, &end, 10);
			if (end == s)
				return -1;
		}
		if (a < 0 || b > 63 || a > b)
			return -1;
		for (; a <= b; a++)
			*mask |= 1ULL << a;
		s = end;
		if (*s == ',')
			s++;
		else if (*s)
			return -1;
	}
	return 0;
}

/* One past the highest pfn of any zone, from /proc/zoneinfo. */
static unsigned long long read_max_pfn(void)
{
	unsigned long long spanned = 0, start, max = 0;
	char line[256];
	FILE *f = fopen("/proc/zoneinfo", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " spanned %llu", &spanned) == 1)
			continue;
		if (sscanf(line, " start_pfn: %llu", &start) == 1 &&
		    start + spanned > max)
			max = start + spanned;
	}
	fclose(f);
	return max;
}

/* Run the engine library's te_init program, if the policy has one. */
static int run_te_init(struct bpf_object *obj, unsigned long long max_pfn)
{
	struct { __u64 max_pfn; } args = { max_pfn };
	LIBBPF_OPTS(bpf_test_run_opts, opts,
		.ctx_in = &args,
		.ctx_size_in = sizeof(args),
	);
	struct bpf_program *prog;
	int err;

	prog = bpf_object__find_program_by_name(obj, "te_init");
	if (!prog)
		return 0;
	if (!max_pfn) {
		fprintf(stderr, "cannot read the highest pfn from /proc/zoneinfo\n");
		return -EINVAL;
	}
	err = bpf_prog_test_run_opts(bpf_program__fd(prog), &opts);
	if (err)
		return err;
	if (opts.retval) {
		fprintf(stderr, "te_init failed: %d\n", (int)opts.retval);
		return opts.retval;
	}
	return 0;
}

/* Run the policy's te_periodic program, if it has one. */
static void run_periodic(struct bpf_program *prog)
{
	LIBBPF_OPTS(bpf_test_run_opts, opts);

	if (prog)
		bpf_prog_test_run_opts(bpf_program__fd(prog), &opts);
}

/* Find a global variable in the object's .bss/.data/.rodata. */
static void *find_var(struct bpf_object *obj, const char *name,
		      const struct btf_type **type, struct bpf_map **mapp)
{
	struct btf *btf = bpf_object__btf(obj);
	struct bpf_map *map;

	bpf_object__for_each_map(map, obj) {
		const char *mname = bpf_map__name(map);
		const struct btf_var_secinfo *vs;
		const struct btf_type *sec;
		const char *secname;
		size_t size;
		char *data;
		int id, i;

		if (!bpf_map__is_internal(map))
			continue;
		secname = strchr(mname, '.');
		if (!secname)
			continue;
		id = btf__find_by_name_kind(btf, secname, BTF_KIND_DATASEC);
		if (id < 0)
			continue;
		sec = btf__type_by_id(btf, id);
		data = bpf_map__initial_value(map, &size);
		if (!data)
			continue;
		vs = btf_var_secinfos(sec);
		for (i = 0; i < btf_vlen(sec); i++, vs++) {
			const struct btf_type *var = btf__type_by_id(btf, vs->type);

			if (strcmp(btf__name_by_offset(btf, var->name_off), name))
				continue;
			if (type)
				*type = var;
			if (mapp)
				*mapp = map;
			return data + vs->offset;
		}
	}
	return NULL;
}

static int set_var(struct bpf_object *obj, const char *assign)
{
	struct btf *btf = bpf_object__btf(obj);
	const struct btf_type *var;
	char name[128];
	const char *eq;
	long long val;
	void *p;
	__s64 sz;

	eq = strchr(assign, '=');
	if (!eq || eq - assign >= (long)sizeof(name))
		return -EINVAL;
	memcpy(name, assign, eq - assign);
	name[eq - assign] = 0;
	val = strtoll(eq + 1, NULL, 0);

	p = find_var(obj, name, &var, NULL);
	if (!p) {
		fprintf(stderr, "policy has no global variable '%s'\n", name);
		return -ENOENT;
	}
	sz = btf__resolve_size(btf, var->type);
	switch (sz) {
	case 1: *(__u8 *)p = val; break;
	case 2: *(__u16 *)p = val; break;
	case 4: *(__u32 *)p = val; break;
	case 8: *(__u64 *)p = val; break;
	default:
		fprintf(stderr, "variable '%s' has unsupported size %lld\n",
			name, (long long)sz);
		return -EINVAL;
	}
	return 0;
}

/* Write cgroup_id/node_mask into every struct tier_ext_ops of the object. */
static int set_target(struct bpf_object *obj, unsigned long long cgid,
		      unsigned long long mask)
{
	struct btf *btf = bpf_object__btf(obj);
	const struct btf_member *m;
	const struct btf_type *t;
	struct bpf_map *map;
	int id, i, found = 0;
	long off_cg = -1, off_mask = -1;

	id = btf__find_by_name_kind(btf, "tier_ext_ops", BTF_KIND_STRUCT);
	if (id < 0) {
		fprintf(stderr, "object has no struct tier_ext_ops\n");
		return -ENOENT;
	}
	t = btf__type_by_id(btf, id);
	m = btf_members(t);
	for (i = 0; i < btf_vlen(t); i++, m++) {
		const char *n = btf__name_by_offset(btf, m->name_off);

		if (!strcmp(n, "cgroup_id"))
			off_cg = btf_member_bit_offset(t, i) / 8;
		else if (!strcmp(n, "node_mask"))
			off_mask = btf_member_bit_offset(t, i) / 8;
	}
	if (off_cg < 0 || off_mask < 0)
		return -EINVAL;

	bpf_object__for_each_map(map, obj) {
		size_t size;
		char *data;

		if (bpf_map__type(map) != BPF_MAP_TYPE_STRUCT_OPS)
			continue;
		data = bpf_map__initial_value(map, &size);
		if (!data)
			return -EINVAL;
		*(__u64 *)(data + off_cg) = cgid;
		*(__u64 *)(data + off_mask) = mask;
		found++;
	}
	return found ? 0 : -ENOENT;
}

/* Print the policy's scalar globals and the non-zero entries of arrays. */
static void dump_vars(struct bpf_object *obj)
{
	struct btf *btf = bpf_object__btf(obj);
	struct bpf_map *map;

	bpf_object__for_each_map(map, obj) {
		const char *mname = bpf_map__name(map);
		const struct btf_var_secinfo *vs;
		const struct btf_type *sec;
		size_t size;
		char *data;
		int id, i;

		if (!bpf_map__is_internal(map) || !strchr(mname, '.') ||
		    strstr(mname, ".rodata") || strstr(mname, ".kconfig"))
			continue;
		id = btf__find_by_name_kind(btf, strchr(mname, '.'), BTF_KIND_DATASEC);
		if (id < 0)
			continue;
		sec = btf__type_by_id(btf, id);
		data = bpf_map__initial_value(map, &size);
		if (!data)
			continue;
		vs = btf_var_secinfos(sec);
		for (i = 0; i < btf_vlen(sec); i++, vs++) {
			const struct btf_type *var = btf__type_by_id(btf, vs->type);
			const char *name = btf__name_by_offset(btf, var->name_off);
			const struct btf_type *vt;
			int tid = btf__resolve_type(btf, var->type);
			unsigned int j;

			if (tid < 0)
				continue;
			vt = btf__type_by_id(btf, tid);
			if (btf_is_array(vt)) {
				const struct btf_array *arr = btf_array(vt);
				__s64 esz = btf__resolve_size(btf, arr->type);

				if (esz != 8)
					continue;
				for (j = 0; j < arr->nelems; j++) {
					__u64 v = ((__u64 *)(data + vs->offset))[j];

					if (v)
						printf("%s[%u] = %llu\n", name, j,
						       (unsigned long long)v);
				}
			} else if (btf_is_int(vt)) {
				__s64 sz = btf__resolve_size(btf, tid);
				long long v = 0;

				if (sz == 8)
					v = *(__s64 *)(data + vs->offset);
				else if (sz == 4)
					v = *(__s32 *)(data + vs->offset);
				else
					continue;
				printf("%s = %lld\n", name, v);
			}
		}
	}
}

int main(int argc, char **argv)
{
	const char *cgdir = NULL, *sets[MAX_SETS], *path;
	unsigned long long cgid = 0, mask = 0, max_pfn;
	int nsets = 0, quiet = 0, opt, err, status = 0;
	struct bpf_program *periodic;
	struct bpf_object *obj;
	struct bpf_link *links[16];
	struct bpf_map *map;
	int nlinks = 0, i;
	char **cmd = NULL;
	pid_t child = 0;

	while ((opt = getopt(argc, argv, "+c:i:n:s:qh")) != -1) {
		switch (opt) {
		case 'c': cgdir = optarg; break;
		case 'i': cgid = strtoull(optarg, NULL, 0); break;
		case 'n':
			if (parse_nodes(optarg, &mask)) {
				fprintf(stderr, "bad node list '%s'\n", optarg);
				return 2;
			}
			break;
		case 's':
			if (nsets == MAX_SETS)
				usage(argv[0]);
			sets[nsets++] = optarg;
			break;
		case 'q': quiet = 1; break;
		default: usage(argv[0]);
		}
	}
	if (optind >= argc)
		usage(argv[0]);
	path = argv[optind++];
	if (optind < argc) {
		if (strcmp(argv[optind], "--"))
			usage(argv[0]);
		if (optind + 1 < argc)
			cmd = &argv[optind + 1];
	}

	if (cgdir) {
		struct stat st;

		/* The cgroup id of a cgroup v2 directory is its inode number. */
		if (stat(cgdir, &st)) {
			perror(cgdir);
			return 1;
		}
		cgid = st.st_ino;
	}

	obj = bpf_object__open_file(path, NULL);
	if (!obj) {
		fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
		return 1;
	}
	err = set_target(obj, cgid, mask);
	if (err) {
		fprintf(stderr, "failed to set policy target: %s\n", strerror(-err));
		return 1;
	}
	if (find_var(obj, "expect_cgroup", NULL, NULL) && cgid) {
		char buf[64];

		snprintf(buf, sizeof(buf), "expect_cgroup=%llu", cgid);
		set_var(obj, buf);
	}
	max_pfn = read_max_pfn();
	if (find_var(obj, "max_pfn", NULL, NULL) && max_pfn) {
		char buf[64];

		snprintf(buf, sizeof(buf), "max_pfn=%llu", max_pfn);
		set_var(obj, buf);
	}
	for (i = 0; i < nsets; i++)
		if (set_var(obj, sets[i]))
			return 1;

	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "failed to load %s: %s\n", path, strerror(-err));
		return 1;
	}
	err = run_te_init(obj, max_pfn);
	if (err) {
		fprintf(stderr, "failed to initialise %s: %s\n", path, strerror(-err));
		return 1;
	}

	bpf_object__for_each_map(map, obj) {
		struct bpf_link *link;

		if (bpf_map__type(map) != BPF_MAP_TYPE_STRUCT_OPS)
			continue;
		link = bpf_map__attach_struct_ops(map);
		if (!link) {
			err = -errno;
			fprintf(stderr, "failed to attach %s: %s\n",
				bpf_map__name(map), strerror(-err));
			goto out;
		}
		links[nlinks++] = link;
		fprintf(stderr, "tier_ext: attached %s to cgroup %llu nodes 0x%llx\n",
			bpf_map__name(map), cgid, mask);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	periodic = bpf_object__find_program_by_name(obj, "te_periodic");
	if (cmd) {
		child = fork();
		if (child == 0) {
			execvp(cmd[0], cmd);
			perror(cmd[0]);
			_exit(127);
		}
		for (i = 0;; i++) {
			pid_t r = waitpid(child, &status, periodic ? WNOHANG : 0);

			if (r == child)
				break;
			if (r < 0 && errno != EINTR)
				break;
			if (r == 0) {	/* still running: te_periodic every ~1s */
				if (i % 20 == 0)
					run_periodic(periodic);
				usleep(50 * 1000);
			}
		}
		status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	} else {
		while (!stop) {
			if (periodic) {
				run_periodic(periodic);
				sleep(1);
			} else {
				pause();
			}
		}
	}

out:
	for (i = 0; i < nlinks; i++)
		bpf_link__destroy(links[i]);
	if (!quiet && nlinks)
		dump_vars(obj);
	bpf_object__close(obj);
	return err ? 1 : status;
}
