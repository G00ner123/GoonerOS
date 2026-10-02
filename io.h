#ifndef IO_H
#define IO_H

static inline void outw(unsigned short port, unsigned short val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(unsigned short port, unsigned char val) {
    asm volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline int strcmp(const char* a, const char* b) {
    while(*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

static inline int strncmp(const char* a, const char* b, int n) {
    for(int i = 0; i < n; i++) {
        if(a[i] != b[i]) return (int)a[i] - (int)b[i];
        if(!a[i]) break;
    }
    return 0;
}

static inline int strlen(const char* s) { int i=0; while(s[i]) i++; return i; }

static inline const char* my_strstr(const char* hay, const char* needle) {
    if(!*needle) return hay;
    for(int i = 0; hay[i]; i++) {
        int j = 0;
        while(needle[j] && hay[i+j] == needle[j]) j++;
        if(!needle[j]) return &hay[i];
    }
    return 0;
}

#endif
