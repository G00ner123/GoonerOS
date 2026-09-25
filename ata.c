#include "kernel.h"
#include "io.h"

static int ata_wait_bsy(void) {
    unsigned int timeout = 1000000;
    while((inb(ATA_STATUS) & 0x80) && --timeout);
    return timeout != 0;
}
static int ata_wait_drq(void) {
    unsigned int timeout = 1000000;
    while(!(inb(ATA_STATUS) & 0x08) && --timeout);
    return timeout != 0;
}
int ata_rw_sectors(unsigned int lba, unsigned short count, unsigned short* buf, int write) {
    if(!buf || count == 0 || count > 255 || lba >= 2048 || lba + count > 2048) return 0;
    if(!ata_wait_bsy()) return 0;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCNT, count);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
    outb(ATA_CMD, write? ATA_WRITE : ATA_READ);
    for(int j = 0; j < count; j++) {
        if(!ata_wait_bsy() || !ata_wait_drq()) return 0;
        if(inb(ATA_STATUS) & 0x01) return 0;
        for(int i = 0; i < 256; i++) {
            if(write) {
    outw(ATA_DATA, buf[j*256 + i]);
} else {
    buf[j*256 + i] = inw(ATA_DATA);
}
        }
    }
    return 1;
}
