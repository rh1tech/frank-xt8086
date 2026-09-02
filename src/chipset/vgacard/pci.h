/*
 * Stub for murm386's PCI header.
 *
 * Only vga_pci_init() needs it, and an XT has no PCI bus. The function is
 * compiled but never called.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct PCIDevice PCIDevice;
typedef struct PCIBus PCIBus;

static inline PCIDevice *pci_register_device(PCIBus *b, const char *name, int devfn,
                                             uint16_t vid, uint16_t did,
                                             uint8_t rev, uint16_t class_id) {
    (void)b; (void)name; (void)devfn; (void)vid; (void)did; (void)rev; (void)class_id;
    return 0;
}

static inline void pci_register_bar(PCIDevice *d, unsigned int bar_num,
                                    uint32_t size, int type, void *opaque,
                                    void *set_addr) {
    (void)d; (void)bar_num; (void)size; (void)type; (void)opaque; (void)set_addr;
}

#define PCI_ADDRESS_SPACE_MEM 0x00
