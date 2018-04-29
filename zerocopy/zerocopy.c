#include "../lib/zerocopy.h"
#include "../common/helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
        if (argc < 2) {
                printf("%s <num of pages> <verbose>\n", argv[0]);
                return 1;
        }
        unsigned long num_pages = atol(argv[1]);
        int verbose = (argc == 3);
        int rounds = verbose ? 1 : 1000000 / num_pages;
        long ret;

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
        long *phys_addrs = malloc(sizeof(long) * num_pages);
        long *orig_phys_addr = malloc(sizeof(long) * num_pages);

        char *virt = aligned_alloc(PAGE_SIZE, num_pages * PAGE_SIZE);
        memset(virt, 0, num_pages * PAGE_SIZE);

        TimingInit();
        printf("Calibrating clocks...\n");
        InitRdtsc();

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                for (int i=0; i<num_pages; i++) {
                        long phys = virt2phys((unsigned long)virt + PAGE_SIZE * i);
                        if (verbose)
                                printf("virt2phys[%d]: %lx\n", i, phys);
                        if (phys < 0)
                                return 3;
                }
        }
        printf("virt2phys done: %ld ns\n", TimingEnd() / rounds / num_pages);

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                virt2physv((unsigned long)virt, phys_addrs, num_pages);
                for (int i=0; i<num_pages; i++) {
                        if (verbose)
                                printf("virt2physv[%d]: %lx\n", i, phys_addrs[i]);
                        if (phys_addrs[i] < 0)
                                return 3;
                }
        }
        printf("virt2physv done: %ld ns\n", TimingEnd() / rounds / num_pages);

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                for (int i=0; i<num_pages; i+=16) {
                        virt2phys16((unsigned long)virt + PAGE_SIZE * i, &phys_addrs[i], (num_pages - i < 16 ? num_pages - i : 16));
                }
                for (int i=0; i<num_pages; i++) {
                        if (verbose)
                                printf("virt2phys16[%d]: %lx\n", i, phys_addrs[i]);
                        if (phys_addrs[i] < 0)
                                return 3;
                }
        }
        printf("virt2phys16 done: %ld ns\n", TimingEnd() / rounds / num_pages);

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                for (int i=0; i<num_pages; i++) {
                        orig_phys_addr[i] = map_phys((unsigned long)virt + PAGE_SIZE * i, phys_addr + i);
                        if (verbose)
                                printf("map_phys[%d]: %lx\n", i, orig_phys_addr[i]);
                        if (orig_phys_addr[i] < 0)
                                return 4;
                }
        }
        printf("First round map_phys done: %ld ns\n", TimingEnd() / rounds / num_pages);

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                for (int i=0; i<num_pages; i++) {
                        long new_phys = map_phys((unsigned long)virt + PAGE_SIZE * i, orig_phys_addr[i]);
                        if (verbose)
                                printf("map_phys[%d]: %lx\n", i, new_phys);
                        if (new_phys < 0)
                                return 4;
                }
        }
        printf("Second round map_phys done: %ld ns\n", TimingEnd() / rounds / num_pages);

        long *ret_phys_addr = malloc(sizeof(long) * num_pages);
        long *phys_addr_list = malloc(sizeof(long) * num_pages);

        memset(ret_phys_addr, 0, sizeof(long) * num_pages);
        for (int i=0; i<num_pages; i++)
                phys_addr_list[i] = phys_addr + i;

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                ret = map_physv((unsigned long)virt, phys_addr_list, ret_phys_addr, num_pages);
                if (ret != 0) {
                        printf("Error: map_physv return %ld\n", ret);
                        return 5;
                }
                if (verbose) {
                        for (int i=0; i<num_pages; i++) {
                                printf("map_physv[%d]: %ld\n", i, ret_phys_addr[i]);
                        }
                }
        }
        printf("First round map_physv done: %ld ns\n", TimingEnd() / rounds / num_pages);

        TimingBegin();
        for (int r=0; r<rounds; r++) {
                ret = map_physv((unsigned long)virt, ret_phys_addr, orig_phys_addr, num_pages);
                if (ret != 0) {
                        printf("Error: map_physv return %ld\n", ret);
                        return 5;
                }
        }
        printf("Second round map_physv done: %ld ns\n", TimingEnd() / rounds / num_pages);

        printf("Finish!\n");
        return 0;
}
