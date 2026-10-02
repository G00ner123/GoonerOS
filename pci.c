#include "kernel.h"
#include "io.h"

static void pci_outl(unsigned short port, unsigned int value) {
    asm volatile("outl %0, %1" :: "a"(value), "Nd"(port));
}

static unsigned int pci_inl(unsigned short port) {
    unsigned int value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static unsigned int pci_read(unsigned int bus, unsigned int device,
                             unsigned int function, unsigned int offset) {
    unsigned int address = 0x80000000u | (bus << 16) | (device << 11) |
                           (function << 8) | (offset & 0xFCu);
    pci_outl(0xCF8, address);
    return pci_inl(0xCFC);
}

static void pci_print_hex(unsigned int value, int digits) {
    static const char hex[] = "0123456789abcdef";
    for(int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        vga_putc(hex[(value >> shift) & 0xFu]);
}

static const char* pci_class_name(unsigned int class_code) {
    switch(class_code) {
        case 0x01: return "Mass storage";
        case 0x02: return "Network controller";
        case 0x03: return "Display controller";
        case 0x04: return "Multimedia controller";
        case 0x05: return "Memory controller";
        case 0x06: return "Bridge";
        case 0x07: return "Communication controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input controller";
        case 0x0A: return "Docking station";
        case 0x0B: return "Processor";
        case 0x0C: return "Serial bus controller";
        case 0x0D: return "Wireless controller";
        case 0x0E: return "Intelligent controller";
        case 0x0F: return "Satellite controller";
        case 0x10: return "Encryption controller";
        case 0x11: return "Signal processing";
        default: return "Unclassified device";
    }
}

void pci_list_devices(void) {
    unsigned int count = 0;
    vga_print("PCI devices (bus:device.function vendor:device class)\n");
    for(unsigned int bus = 0; bus < 256; bus++) {
        for(unsigned int device = 0; device < 32; device++) {
            unsigned int identity = pci_read(bus, device, 0, 0);
            if((identity & 0xFFFFu) == 0xFFFFu) continue;
            unsigned int header = (pci_read(bus, device, 0, 0x0C) >> 16) & 0xFFu;
            unsigned int function_count = (header & 0x80u) ? 8 : 1;
            for(unsigned int function = 0; function < function_count; function++) {
                if(function) {
                    identity = pci_read(bus, device, function, 0);
                    if((identity & 0xFFFFu) == 0xFFFFu) continue;
                }
                unsigned int class_register = pci_read(bus, device, function, 0x08);
                unsigned int class_code = class_register >> 24;
                pci_print_hex(bus, 2);
                vga_putc(':');
                pci_print_hex(device, 2);
                vga_putc('.');
                pci_print_hex(function, 1);
                vga_putc(' ');
                pci_print_hex(identity & 0xFFFFu, 4);
                vga_putc(':');
                pci_print_hex(identity >> 16, 4);
                vga_putc(' ');
                vga_print(pci_class_name(class_code));
                vga_print(" [");
                pci_print_hex(class_code, 2);
                pci_print_hex((class_register >> 16) & 0xFFu, 2);
                vga_print("]\n");
                count++;
            }
        }
    }
    if(!count) vga_print("No PCI devices found or PCI configuration access is unavailable.\n");
    else {
        print_int((int)count);
        vga_print(" device(s)\n");
    }
}
