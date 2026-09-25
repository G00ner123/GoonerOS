#include "kernel.h"
#include "io.h"

unsigned char cmos_read(unsigned char reg) {
    outb(0x70, reg);
    return inb(0x71);
}

unsigned char bcd_to_bin(unsigned char bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static int cmos_update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}
void read_rtc_raw(unsigned char* h, unsigned char* m, unsigned char* s,
                          unsigned char* d, unsigned char* mo, unsigned char* y) {
    while(cmos_update_in_progress());
    *s = cmos_read(0x00);
    *m = cmos_read(0x02);
    *h = cmos_read(0x04);
    *d = cmos_read(0x07);
    *mo = cmos_read(0x08);
    *y = cmos_read(0x09);
}
void print_time_date(void) {
    unsigned char s,m,h,d,mo,y, s2,m2,h2,d2,mo2,y2;

    // Zweimal lesen und vergleichen, falls die Uhr genau waehrend des Auslesens weiterspringt
    do {
        read_rtc_raw(&h,&m,&s,&d,&mo,&y);
        read_rtc_raw(&h2,&m2,&s2,&d2,&mo2,&y2);
    } while(h!=h2 || m!=m2 || s!=s2 || d!=d2 || mo!=mo2 || y!=y2);

    unsigned char regB = cmos_read(0x0B);
    if(!(regB & 0x04)) { // Bit 2 = 0 -> Werte liegen als BCD vor, erst dann umrechnen
        s = bcd_to_bin(s); m = bcd_to_bin(m);
        d = bcd_to_bin(d); mo = bcd_to_bin(mo); y = bcd_to_bin(y);
        h = bcd_to_bin(h & 0x7F) | (h & 0x80);
    }
    if(!(regB & 0x02) && (h & 0x80)) { // 12-Stunden-Format mit gesetztem PM-Bit
        h = ((h & 0x7F) + 12) % 24;
    } else {
        h &= 0x7F;
    }
    char buf[32];
    buf[0]='0'+h/10; buf[1]='0'+h%10; buf[2]=':';
    buf[3]='0'+m/10; buf[4]='0'+m%10; buf[5]=':';
    buf[6]='0'+s/10; buf[7]='0'+s%10; buf[8]=' '; buf[9]=0;
    vga_print(buf);
    buf[0]='0'+d/10; buf[1]='0'+d%10; buf[2]='/';
    buf[3]='0'+mo/10; buf[4]='0'+mo%10; buf[5]='/';
    buf[6]='0'+y/10; buf[7]='0'+y%10; buf[8]='\n'; buf[9]=0;
    vga_print(buf);
}
