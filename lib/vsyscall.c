#include <inc/vsyscall.h>
#include <inc/lib.h>

static inline uint64_t
vsyscall(int num) {
    // LAB 12: Your code here DONE
    (void)num;
    return num < NSYSCALLS ? (int)vsys[num] : -E_INVAL;
}

int
vsys_gettime(void) {
    return vsyscall(VSYS_gettime);
}
