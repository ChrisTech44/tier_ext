// SPDX-License-Identifier: GPL-2.0
/*
 * te_resident FILE [PARTS]: print which fraction of FILE is in the page
 * cache, split into PARTS equal ranges (default 3), as percentages:
 *
 *   $ te_resident big 3
 *   97 0 1
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	long page = sysconf(_SC_PAGESIZE), n, i, parts;
	unsigned char *vec;
	struct stat st;
	void *m;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE [PARTS]\n", argv[0]);
		return 2;
	}
	parts = argc > 2 ? atol(argv[2]) : 3;
	fd = open(argv[1], O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || !st.st_size || parts < 1) {
		perror(argv[1]);
		return 1;
	}
	n = (st.st_size + page - 1) / page;
	m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	vec = malloc(n);
	if (m == MAP_FAILED || !vec || mincore(m, st.st_size, vec)) {
		perror("mincore");
		return 1;
	}
	for (i = 0; i < parts; i++) {
		long a = n * i / parts, b = n * (i + 1) / parts, j, r = 0;

		for (j = a; j < b; j++)
			r += vec[j] & 1;
		printf("%s%ld", i ? " " : "", b > a ? r * 100 / (b - a) : 0);
	}
	printf("\n");
	return 0;
}
