#include <stdint.h>
#include <stddef.h>
#include <lib/klib.h>
#include <acpi/acpi.h>
#include <acpi/madt.h>
#include <lai/core.h>
#include <mm/mm.h>

int acpi_available = 0;

static int use_xsdt = 0;

struct rsdp_t *rsdp;
struct rsdt_t *rsdt;
struct xsdt_t *xsdt;

/* This function should look for all the ACPI tables and index them for
   later use */
void init_acpi(void) {
    kprint(KPRN_INFO, "acpi: Initialising...");

    /* look for the "RSD PTR " signature from 0x80000 to 0xa0000 and from
       0xf0000 to 0x100000 */
    for (size_t i = 0x80000 + MEM_PHYS_OFFSET; i < 0x100000 + MEM_PHYS_OFFSET; i += 16) {
        if (i == 0xa0000 + MEM_PHYS_OFFSET) {
            /* skip video mem and mapped hardware */
            i = 0xe0000 - 16 + MEM_PHYS_OFFSET;
            continue;
        }
        if (!kstrncmp((char *)i, "RSD PTR ", 8)) {
            kprint(KPRN_INFO, "acpi: Found RSDP at %X", i);
            rsdp = (struct rsdp_t *)i;
            goto rsdp_found;
        }
    }
    acpi_available = 0;
    kprint(KPRN_INFO, "acpi: Non-ACPI compliant system");
    return;

rsdp_found:
    acpi_available = 1;
    kprint(KPRN_INFO, "acpi: ACPI available");

    kprint(KPRN_INFO, "acpi: Revision: %u", (uint32_t)rsdp->rev);

    if (rsdp->rev >= 2 && rsdp->xsdt_addr) {
        use_xsdt = 1;
        kprint(KPRN_INFO, "acpi: Found XSDT at %X", ((size_t)rsdp->xsdt_addr + MEM_PHYS_OFFSET));
        xsdt = (struct xsdt_t *)((size_t)rsdp->xsdt_addr + MEM_PHYS_OFFSET);
    } else {
        kprint(KPRN_INFO, "acpi: Found RSDT at %X", ((size_t)rsdp->rsdt_addr + MEM_PHYS_OFFSET));
        rsdt = (struct rsdt_t *)((size_t)rsdp->rsdt_addr + MEM_PHYS_OFFSET);
    }

    struct sdt_t *ptr;

    if (use_xsdt) {
        kprint(KPRN_INFO, "acpi: Found %u tables", xsdt->sdt.length);
        for (size_t i = 0; i < xsdt->sdt.length; i++) {
            ptr = (struct sdt_t *)((size_t)xsdt->sdt_ptr[i] + MEM_PHYS_OFFSET);
            kprint(KPRN_INFO, "acpi: Found %s at %X", (const char *)ptr->signature, (size_t)ptr);
        }
    } else {
        kprint(KPRN_INFO, "acpi: Found %u tables", rsdt->sdt.length);
        for (size_t i = 0; i < rsdt->sdt.length; i++) {
            ptr = (struct sdt_t *)((size_t)rsdt->sdt_ptr[i] + MEM_PHYS_OFFSET);
            kprint(KPRN_INFO, "acpi: Found %s at %X", (const char *)ptr->signature, (size_t)ptr);
        }
    }

    /* Call table inits */
    init_madt();
    void *dsdt = acpi_find_sdt("DSDT");
    if (dsdt)
        lai_create_namespace(dsdt);
    else
        kprint(KPRN_INFO, "lai: Could not find DSDT. AML namespace management will not be available");

    return;
}

/* Find SDT by signature */
void *acpi_find_sdt(const char *signature) {
    struct sdt_t *ptr;

    if (use_xsdt) {
        for (size_t i = 0; i < xsdt->sdt.length; i++) {
            ptr = (struct sdt_t *)((size_t)xsdt->sdt_ptr[i] + MEM_PHYS_OFFSET);
            if (!kstrncmp(ptr->signature, signature, 4)) {
                kprint(KPRN_INFO, "acpi: Found \"%s\" at %X", signature, (size_t)ptr);
                return (void *)ptr;
            }
        }
    } else {
        for (size_t i = 0; i < rsdt->sdt.length; i++) {
            ptr = (struct sdt_t *)((size_t)rsdt->sdt_ptr[i] + MEM_PHYS_OFFSET);
            if (!kstrncmp(ptr->signature, signature, 4)) {
                kprint(KPRN_INFO, "acpi: Found \"%s\" at %X", signature, (size_t)ptr);
                return (void *)ptr;
            }
        }
    }

    kprint(KPRN_INFO, "acpi: \"%s\" not found", signature);
    return (void *)0;
}
