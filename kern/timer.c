#include <inc/types.h>
#include <inc/assert.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/stdio.h>
#include <inc/x86.h>
#include <inc/uefi.h>
#include <kern/timer.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/trap.h>

#define kilo      (1000ULL)
#define Mega      (kilo * kilo)
#define Giga      (kilo * Mega)
#define Tera      (kilo * Giga)
#define Peta      (kilo * Tera)
#define ULONG_MAX ~0UL

#if LAB <= 6
/* Early variant of memory mapping that does 1:1 aligned area mapping
 * in 2MB pages. You will need to reimplement this code with proper
 * virtual memory mapping in the future. */
void *
mmio_map_region(physaddr_t pa, size_t size) {
    void map_addr_early_boot(uintptr_t addr, uintptr_t addr_phys, size_t sz);
    const physaddr_t base_2mb = 0x200000;
    uintptr_t org = pa;
    size += pa & (base_2mb - 1);
    size += (base_2mb - 1);
    pa &= ~(base_2mb - 1);
    size &= ~(base_2mb - 1);
    map_addr_early_boot(pa, pa, size);
    return (void *)org;
}
void *
mmio_remap_last_region(physaddr_t pa, void *addr, size_t oldsz, size_t newsz) {
    return mmio_map_region(pa, newsz);
}
#endif

struct Timer timertab[MAX_TIMERS];
struct Timer *timer_for_schedule;

struct Timer timer_hpet0 = {
        .timer_name = "hpet0",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim0,
        .handle_interrupts = hpet_handle_interrupts_tim0,
};

struct Timer timer_hpet1 = {
        .timer_name = "hpet1",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim1,
        .handle_interrupts = hpet_handle_interrupts_tim1,
};

struct Timer timer_acpipm = {
        .timer_name = "pm",
        .timer_init = acpi_enable,
        .get_cpu_freq = pmtimer_cpu_frequency,
};

void
acpi_enable(void) {
    FADT *fadt = get_fadt();
    outb(fadt->SMI_CommandPort, fadt->AcpiEnable);
    while ((inw(fadt->PM1aControlBlock) & 1) == 0) /* nothing */
        ;
}

uint8_t
checksum(uint8_t *ptr, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++, ptr++) sum += *ptr;
    return (uint8_t) sum & 0xFFU; 
}


RSDP *
get_rsdp(void) {
    RSDP *rsd_ptr = (RSDP *)mmio_map_region((physaddr_t)uefi_lp->ACPIRoot, sizeof(RSDP));

    if (checksum((uint8_t *) rsd_ptr, 20)) {
        panic("get_rsdp: invalid RSDP checksum\n");
    }
    if (strncmp(rsd_ptr->Signature, "RSD PTR ", 8)) {
        panic("get_rsdp: invalid RSDP signature\n");
    }

    if (rsd_ptr->Revision >= 2) {
        if (checksum((uint8_t *) rsd_ptr, 36)) {
            panic("get_rsdp: invalid XSDT checksum\n");
        } 
    }

    return rsd_ptr;
}

static void *
acpi_find_table(const char *sign) {
    /*
     * This function performs lookup of ACPI table by its signature
     * and returns valid pointer to the table mapped somewhere.
     *
     * It is a good idea to checksum tables before using them.
     *
     * HINT: Use mmio_map_region/mmio_remap_last_region
     * before accessing table addresses
     * (Why mmio_remap_last_region is requrired?)
     * HINT: RSDP address is stored in uefi_lp->ACPIRoot
     * HINT: You may want to distunguish RSDT/XSDT
     */
    // LAB 5: Your code here:


    RSDP *rsd_ptr = get_rsdp();
    RSDT *rsdt_ptr;

    if (rsd_ptr->Revision >= 2) {
        rsdt_ptr = (RSDT *)mmio_map_region((physaddr_t)rsd_ptr->XsdtAddress, sizeof(RSDT));
        if (strncmp(rsdt_ptr->h.Signature, "XSDT", 4)) {
            panic("acpi_find_table: invalid XSDT signature\n");
        }
        rsdt_ptr = (RSDT *)mmio_remap_last_region((physaddr_t)(rsd_ptr->XsdtAddress), (void *) (rsd_ptr->XsdtAddress), sizeof(RSDT), rsdt_ptr->h.Length);
    } else {
        rsdt_ptr = (RSDT *)mmio_map_region((physaddr_t)rsd_ptr->RsdtAddress, sizeof(RSDT));
        if (strncmp(rsdt_ptr->h.Signature, "RSDT", 4)) {
            panic("acpi_find_table: invalid RSDT signature\n");
        }
        rsdt_ptr = (RSDT *)mmio_remap_last_region((physaddr_t)(rsd_ptr->RsdtAddress), (void *) (rsd_ptr->RsdtAddress), sizeof(RSDT), rsdt_ptr->h.Length);
    }

    if (checksum((uint8_t *) rsdt_ptr, rsdt_ptr->h.Length)) {
        panic("acpi_find_table: invalid RSDT checksum\n");
    }
    
    size_t sdt_num = rsdt_ptr->h.Length - sizeof(ACPISDTHeader);
    if (rsd_ptr->Revision >= 2) {
        sdt_num /= 8;
    } else {
        sdt_num /= 4;
    }

    for (size_t i = 0; i < sdt_num; i++) {
        ACPISDTHeader *hdr = (ACPISDTHeader *)mmio_map_region(rsdt_ptr->PointerToOtherSDT[i], sizeof(ACPISDTHeader));
        if (!strncmp(hdr->Signature, sign, 4)) {
            return hdr;
        }
    }

    return NULL;
}

/* Obtain and map FADT ACPI table address. */
FADT *
get_fadt(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    // HINT: ACPI table signatures are
    //       not always as their names
    FADT *fadt_ptr = acpi_find_table("FACP");
    if (!fadt_ptr) {
        panic("get_fadt: couldn't resolve FADT\n");
    }

    fadt_ptr = (FADT *)mmio_remap_last_region((physaddr_t) fadt_ptr, (void *)fadt_ptr, sizeof(ACPISDTHeader), fadt_ptr->h.Length);
    if (checksum((uint8_t *) fadt_ptr, fadt_ptr->h.Length)) {
        panic("get_fadt: invalid FADT checksum\n");
    }

    return fadt_ptr;
}

/* Obtain and map RSDP ACPI table address. */
HPET *
get_hpet(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    HPET *hpet_ptr = acpi_find_table("HPET");
    if (!hpet_ptr) {
        panic("get_hpet: couldn't resolve HPET\n");
    }
    hpet_ptr = (HPET *)mmio_remap_last_region((physaddr_t) hpet_ptr, (void *)hpet_ptr, sizeof(ACPISDTHeader), hpet_ptr->h.Length);
    if (checksum((uint8_t *) hpet_ptr, hpet_ptr->h.Length)) {
        panic("get_hpet: invalid HPET checksum\n");
    } 
    return hpet_ptr;
}

/* Getting physical HPET timer address from its table. */
HPETRegister *
hpet_register(void) {
    HPET *hpet_timer = get_hpet();
    if (!hpet_timer->address.address) panic("hpet is unavailable\n");

    uintptr_t paddr = hpet_timer->address.address;
    return mmio_map_region(paddr, sizeof(HPETRegister));
}

/* Debug HPET timer state. */
void
hpet_print_struct(void) {
    HPET *hpet = get_hpet();
    assert(hpet != NULL);
    cprintf("signature = %s\n", (hpet->h).Signature);
    cprintf("length = %08x\n", (hpet->h).Length);
    cprintf("revision = %08x\n", (hpet->h).Revision);
    cprintf("checksum = %08x\n", (hpet->h).Checksum);

    cprintf("oem_revision = %08x\n", (hpet->h).OEMRevision);
    cprintf("creator_id = %08x\n", (hpet->h).CreatorID);
    cprintf("creator_revision = %08x\n", (hpet->h).CreatorRevision);

    cprintf("hardware_rev_id = %08x\n", hpet->hardware_rev_id);
    cprintf("comparator_count = %08x\n", hpet->comparator_count);
    cprintf("counter_size = %08x\n", hpet->counter_size);
    cprintf("reserved = %08x\n", hpet->reserved);
    cprintf("legacy_replacement = %08x\n", hpet->legacy_replacement);
    cprintf("pci_vendor_id = %08x\n", hpet->pci_vendor_id);
    cprintf("hpet_number = %08x\n", hpet->hpet_number);
    cprintf("minimum_tick = %08x\n", hpet->minimum_tick);

    cprintf("address_structure:\n");
    cprintf("address_space_id = %08x\n", (hpet->address).address_space_id);
    cprintf("register_bit_width = %08x\n", (hpet->address).register_bit_width);
    cprintf("register_bit_offset = %08x\n", (hpet->address).register_bit_offset);
    cprintf("address = %08lx\n", (unsigned long)(hpet->address).address);
}

static volatile HPETRegister *hpetReg;
/* HPET timer period (in femtoseconds) */
static uint64_t hpetFemto = 0;
/* HPET timer frequency */
static uint64_t hpetFreq = 0;

/* HPET timer initialisation */
void
hpet_init() {
    if (hpetReg == NULL) {
        nmi_disable();
        hpetReg = hpet_register();
        uint64_t cap = hpetReg->GCAP_ID;
        hpetFemto = (uintptr_t)(cap >> 32);
        if (!(cap & HPET_LEG_RT_CAP)) panic("HPET has no LegacyReplacement mode");

        // cprintf("hpetFemto = %llu\n", hpetFemto);
        hpetFreq = (1 * Peta) / hpetFemto;
        // cprintf("HPET: Frequency = %d.%03dMHz\n", (uintptr_t)(hpetFreq / Mega), (uintptr_t)(hpetFreq % Mega));
        /* Enable ENABLE_CNF bit to enable timer */
        hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
        nmi_enable();
    }
}

/* HPET register contents debugging. */
void
hpet_print_reg(void) {
    cprintf("GCAP_ID = %016lx\n", (unsigned long)hpetReg->GCAP_ID);
    cprintf("GEN_CONF = %016lx\n", (unsigned long)hpetReg->GEN_CONF);
    cprintf("GINTR_STA = %016lx\n", (unsigned long)hpetReg->GINTR_STA);
    cprintf("MAIN_CNT = %016lx\n", (unsigned long)hpetReg->MAIN_CNT);
    cprintf("TIM0_CONF = %016lx\n", (unsigned long)hpetReg->TIM0_CONF);
    cprintf("TIM0_COMP = %016lx\n", (unsigned long)hpetReg->TIM0_COMP);
    cprintf("TIM0_FSB = %016lx\n", (unsigned long)hpetReg->TIM0_FSB);
    cprintf("TIM1_CONF = %016lx\n", (unsigned long)hpetReg->TIM1_CONF);
    cprintf("TIM1_COMP = %016lx\n", (unsigned long)hpetReg->TIM1_COMP);
    cprintf("TIM1_FSB = %016lx\n", (unsigned long)hpetReg->TIM1_FSB);
    cprintf("TIM2_CONF = %016lx\n", (unsigned long)hpetReg->TIM2_CONF);
    cprintf("TIM2_COMP = %016lx\n", (unsigned long)hpetReg->TIM2_COMP);
    cprintf("TIM2_FSB = %016lx\n", (unsigned long)hpetReg->TIM2_FSB);
}

/* HPET main timer counter value. */
uint64_t
hpet_get_main_cnt(void) {
    return hpetReg->MAIN_CNT;
}

/* - Configure HPET timer 0 to trigger every 0.5 seconds on IRQ_TIMER line
 * - Configure HPET timer 1 to trigger every 1.5 seconds on IRQ_CLOCK line
 *
 * HINT To be able to use HPET as PIT replacement consult
 *      LegacyReplacement functionality in HPET spec.
 * HINT Don't forget to unmask interrupt in PIC */
void
hpet_enable_interrupts_tim0(void) {
    // LAB 5: Your code here

    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;

    hpetReg->TIM0_CONF = 0;
    hpetReg->TIM0_CONF |= HPET_TN_TYPE_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_INT_ENB_CNF;
    hpetReg->TIM0_CONF |= HPET_TN_VAL_SET_CNF;
    hpetReg->TIM0_CONF |= (IRQ_TIMER << 9);
    hpetReg->TIM0_CONF = hpetFreq / 2;
    pic_irq_unmask(IRQ_TIMER);
}

void
hpet_enable_interrupts_tim1(void) {
    // LAB 5: Your code here

    hpetReg->GEN_CONF |= HPET_LEG_RT_CNF;
    
    hpetReg->TIM1_CONF = 0;
    hpetReg->TIM1_CONF |= HPET_TN_TYPE_CNF;
    hpetReg->TIM1_CONF |= HPET_TN_INT_ENB_CNF;
    hpetReg->TIM1_CONF |= HPET_TN_VAL_SET_CNF;
    hpetReg->TIM1_CONF |= (IRQ_TIMER << 9);
    hpetReg->TIM1_CONF = 3 * hpetFreq / 2;

    pic_irq_unmask(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim0(void) {
    pic_send_eoi(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim1(void) {
    pic_send_eoi(IRQ_CLOCK);
}

/* Calculate CPU frequency in Hz with the help with HPET timer.
 * HINT Use hpet_get_main_cnt function and do not forget about
 * about pause instruction. */
uint64_t
hpet_cpu_frequency(void) {
    static uint64_t cpu_freq;

    // LAB 5: Your code here

    uint64_t hpet_start = hpet_get_main_cnt();
    uint64_t tsc_start = read_tsc();
    for (size_t i = 0; i < 1000; i++) {
        asm("pause");
    }

    uint64_t hpet_ticks = hpet_get_main_cnt() - hpet_start;
    uint64_t tsc_ticks = read_tsc() - tsc_start;
    cpu_freq = (tsc_ticks * hpetFreq) / hpet_ticks; 

    return cpu_freq;
}

uint32_t
pmtimer_get_timeval(void) {
    FADT *fadt = get_fadt();
    return inl(fadt->PMTimerBlock);
}

/* Calculate CPU frequency in Hz with the help with ACPI PowerManagement timer.
 * HINT Use pmtimer_get_timeval function and do not forget that ACPI PM timer
 *      can be 24-bit or 32-bit. */
uint64_t
pmtimer_cpu_frequency(void) {
    static uint64_t cpu_freq;

    // LAB 5: Your code here
    uint32_t fl24 = (get_fadt()->Flags >> 8) & 1;

    uint64_t pmt_ticks = 0;
    uint64_t pmt_start = pmtimer_get_timeval();
    uint64_t tsc_start = read_tsc();

    for (size_t i = 0; i < 1000; i++) {
        asm("pause");
        uint64_t pmt_cur = pmtimer_get_timeval();

        if (pmt_cur > pmt_start) {
            pmt_ticks += pmt_cur - pmt_start;
        } else {
            if (fl24) {
                pmt_ticks += pmt_cur + 0x00FFFFFF - pmt_start;
            } else {
                pmt_ticks += pmt_cur + 0xFFFFFFFF - pmt_start;
            }
        }

        pmt_start = pmt_cur;
    }

    uint64_t tsc_ticks = read_tsc() - tsc_start;
    cpu_freq = (tsc_ticks * PM_FREQ) / pmt_ticks;

    return cpu_freq;
}
