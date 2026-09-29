// SPDX-License-Identifier: GPL-2.0
/*
 * te_touch MB PASSES: allocate MB of private anonymous memory and write one
 * byte per page, PASSES times; print the elapsed seconds. A minimal anon
 * workload whose time is dominated by faults and, under a memory limit, by
 * reclaim and swap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

int main(int argc, char **argv)
{
	size_t mb = argc > 1 ? strtoul(argv[1], NULL, 0) : 256, len = mb << 20, i;
	int passes = argc > 2 ? atoi(argv[2]) : 1, p;
	struct timespec a, b;
	volatile char *m;

	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (p = 0; p < passes; p++)
		for (i = 0; i < len; i += 4096)
			m[i] = (char)(p + 1);
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("%.3f\n", (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
	return 0;
}
