#include <unistd.h>
#include <sys/syscall.h>

#define PAGE_SIZE (1<<12)
#define MAX_ORDER 11

#define __NR_alloc_phys 333
#define __NR_virt2phys 334
#define __NR_virt2physv 335
#define __NR_map_phys 336
#define __NR_map_physv 337
#define __NR_socksdirect 338
#define __NR_virt2phys16 339

// return physical page number
inline long alloc_phys(unsigned long num_pages) {
    int order = 0;
    while ((unsigned int)(1 << order) < num_pages) {
        order ++;
    }
    if (order > MAX_ORDER) { // too large, cannot succeed
        return -2;
    }
	return syscall(__NR_alloc_phys, order);
}

// return physical page number
inline long virt2phys(unsigned long virt_addr) {
	return syscall(__NR_virt2phys, virt_addr);
}

// return original physical page number
inline long virt2physv(unsigned long virt_addr, unsigned long *phys_addr, unsigned long npages) {
	return syscall(__NR_virt2physv, virt_addr, phys_addr, npages);
}

// return original physical page number
inline long map_phys(unsigned long virt_addr, unsigned long phys_addr) {
	return syscall(__NR_map_phys, virt_addr, phys_addr);
}

// return 0 on success
inline long map_physv(unsigned long virt_addr, unsigned long *phys_addr, unsigned long *old_phys_addr, unsigned long npages) {
	return syscall(__NR_map_physv, virt_addr, phys_addr, old_phys_addr, npages);
}

// echo param
inline long sys_socksdirect(unsigned long echo) {
    return syscall(__NR_socksdirect, echo);
}

// return original physical page number
inline long virt2phys16(unsigned long virt_addr, unsigned long *phys_addr, unsigned long npages) {
	return syscall(__NR_virt2phys16, virt_addr, phys_addr, npages);
}

