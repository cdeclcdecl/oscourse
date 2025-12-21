/* See COPYRIGHT for copyright information. */

#ifndef JOS_KERN_TRAP_H
#define JOS_KERN_TRAP_H
#ifndef JOS_KERNEL
#error "This is a JOS kernel header; user programs should not #include it"
#endif

#include <inc/trap.h>
#include <inc/mmu.h>

/* The kernel's interrupt descriptor table */
extern struct Gatedesc idt[];
extern struct Pseudodesc idt_pd;

extern bool in_page_fault;

void clock_idt_init(void);
void trap_init(void);
void trap_init_percpu(void);
void print_regs(struct PushRegs *regs);
void print_trapframe(struct Trapframe *tf);

void clock_thdlr(void);
void timer_thdlr(void);
void thdlr0(void);
void thdlr1(void);
void thdlr2(void);
void thdlr3(void);
void thdlr4(void);
void thdlr5(void);
void thdlr6(void);
void thdlr7(void);
void thdlr8(void);
void thdlr10(void);
void thdlr11(void);
void thdlr12(void);
void thdlr13(void);
void thdlr14(void);
void thdlr15(void);
void thdlr16(void);
void thdlr17(void);
void thdlr18(void);
void thdlr19(void);
void thdlr48(void);
void kbd_thdlr(void);
void serial_thdlr(void);

#endif /* JOS_KERN_TRAP_H */
