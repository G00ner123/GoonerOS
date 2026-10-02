#include "kernel.h"
#include "io.h"

#define ATA_ALT_STATUS 0x3F6
#define ATA_CACHE_FLUSH 0xE7

static int ata_status_error(unsigned char status) {
    return status == 0 || status == 0xFF || (status & 0x21);
}

static int ata_wait_bsy(void) {
    unsigned int timeout = 1000000;
    unsigned char status;
    do {
        status = inb(ATA_STATUS);
        if(status == 0 || status == 0xFF) return 0;
        if(!(status & 0x80)) return 1;
    } while(--timeout);
    return 0;
}

static int ata_wait_drq(void) {
    unsigned int timeout = 1000000;
    unsigned char status;
    do {
        status = inb(ATA_STATUS);
        if(ata_status_error(status)) return 0;
        if(!(status & 0x80) && (status & 0x08)) return 1;
    } while(--timeout);
    return 0;
}

static int ata_wait_complete(void) {
    unsigned int timeout = 1000000;
    unsigned char status;
    do {
        status = inb(ATA_STATUS);
        if(ata_status_error(status)) return 0;
        if(!(status & 0x88)) return 1;
    } while(--timeout);
    return 0;
}

int ata_rw_sectors(unsigned int lba, unsigned short count, unsigned short* buf, int write) {
    if(!buf || count == 0 || count > 255 || lba >= 2048 || lba + count > 2048) return 0;
    if(!ata_wait_bsy()) return 0;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    outb(ATA_SECCNT, count);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
    outb(ATA_CMD, write? ATA_WRITE : ATA_READ);
    for(int j = 0; j < count; j++) {
        if(!ata_wait_bsy() || !ata_wait_drq()) return 0;
        for(int i = 0; i < 256; i++) {
            if(write) {
                outw(ATA_DATA, buf[j*256 + i]);
            } else {
                buf[j*256 + i] = inw(ATA_DATA);
            }
        }
    }
    if(!ata_wait_complete()) return 0;
    if(write) {
        outb(ATA_CMD, ATA_CACHE_FLUSH);
        if(!ata_wait_complete()) return 0;
    }
    return 1;
}
