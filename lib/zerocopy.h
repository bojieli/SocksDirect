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

static std::unordered_map<unsigned long,unsigned long> page_mappings;

static inline void log_mapping(void *buf, unsigned long *original_pages, int num_pages)
{
        for (int i=0; i<num_pages; i++) {
            unsigned long addr = (unsigned long)(buf) + i * PAGE_SIZE;
            if (page_mappings.count(addr) == 0) {
                page_mappings.insert(std::pair<unsigned long,unsigned long>(addr, original_pages[i]));
            }
        }
}

static inline void init_mapping(void)
{
        const int num_pages = 1;
        alloc_phys(num_pages);
}

static inline void resume_mapping(void)
{
        for (auto& x: page_mappings) {
            if (x.second != 0)
                map_phys(x.first, x.second);
        }
}


