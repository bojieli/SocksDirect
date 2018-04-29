#include "../lib/zerocopy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
	if (argc != 2) {
		printf("%s <num of pages>\n", argv[0]);
		return 1;
	}
	unsigned long num_pages = atol(argv[1]);

    if (sys_socksdirect(num_pages) != num_pages) {
        printf("socksdirect not supported by the kernel, return %ld\n", sys_socksdirect(num_pages));
        return 1;
    }

	printf ("begin alloc phys %ld pages\n", num_pages);
	long phys_addr = alloc_phys(num_pages);
	printf("alloc phys page addr %ld\n", phys_addr);
	if (phys_addr < 0) {
		return 2;
    }
	long *orig_phys_addr = malloc(sizeof(long) * num_pages);

	char *virt = aligned_alloc(PAGE_SIZE, num_pages * PAGE_SIZE);
	memset(virt, 0, num_pages * PAGE_SIZE);

	for (int i=0; i<num_pages; i++) {
        long phys = virt2phys((unsigned long)virt + PAGE_SIZE * i);
		printf("virt2phys[%d]: %lx\n", i, phys);
        if (phys < 0)
            return 3;
	}

	for (int i=0; i<num_pages; i++) {
		orig_phys_addr[i] = map_phys((unsigned long)virt + PAGE_SIZE * i, phys_addr + i);
		printf("map_phys[%d]: %lx\n", i, orig_phys_addr[i]);
		if (orig_phys_addr[i] < 0)
			return 4;
	}

	long *ret_phys_addr = malloc(sizeof(long) * num_pages);
	long ret = map_physv((unsigned long)virt, orig_phys_addr, ret_phys_addr, num_pages);
	if (ret != 0) {
		printf("Error: map_physv return %ld\n", ret);
        return 5;
	}
	for (int i=0; i<num_pages; i++) {
		printf("map_physv[%d]: %ld\n", i, ret_phys_addr[i]);
	}

	return 0;
}
