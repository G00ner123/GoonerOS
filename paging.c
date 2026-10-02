#include "kernel.h"

#define PAGE_SIZE 4096u
#define PAGE_PRESENT 0x001u
#define PAGE_WRITABLE 0x002u
#define PAGE_USER 0x004u
#define PAGE_ADDRESS 0xFFFFF000u
#define IDENTITY_TABLES (PAGING_IDENTITY_LIMIT / (PAGE_SIZE * 1024u))
#define FRAMEBUFFER_TABLES 2u
#define USER_CODE_ADDRESS 0x40000000u
#define USER_STACK_ADDRESS 0x40002000u

extern char _kernel_text_start;
extern char _kernel_ro_end;

static unsigned int page_directory[1024] __attribute__((aligned(4096)));
static unsigned int identity_tables[IDENTITY_TABLES][1024] __attribute__((aligned(4096)));
static unsigned int framebuffer_tables[FRAMEBUFFER_TABLES][1024] __attribute__((aligned(4096)));
static unsigned int user_directories[USER_PROCESS_LIMIT][1024] __attribute__((aligned(4096)));
static unsigned int user_tables[USER_PROCESS_LIMIT][1024] __attribute__((aligned(4096)));
static int paging_active;

int paging_init(unsigned int framebuffer, unsigned int pitch, unsigned int height) {
    unsigned int framebuffer_bytes;
    if(!framebuffer || !pitch || !height || pitch > 0xFFFFFFFFu / height) return 0;
    framebuffer_bytes = pitch * height;
    if(framebuffer_bytes == 0 || framebuffer_bytes - 1u > 0xFFFFFFFFu - framebuffer) return 0;

    for(unsigned int i = 0; i < 1024; i++) page_directory[i] = 0;
    for(unsigned int table = 0; table < IDENTITY_TABLES; table++) {
        unsigned int directory_index = table;
        page_directory[directory_index] =
            (unsigned int)identity_tables[table] | PAGE_PRESENT | PAGE_WRITABLE;
        for(unsigned int entry = 0; entry < 1024; entry++) {
            unsigned int address = (table * 1024u + entry) * PAGE_SIZE;
            unsigned int flags = PAGE_PRESENT | PAGE_WRITABLE;
            if(address >= (unsigned int)&_kernel_text_start && address < (unsigned int)&_kernel_ro_end)
                flags = PAGE_PRESENT;
            identity_tables[table][entry] = address | flags;
        }
    }
    identity_tables[0][0] = 0;

    unsigned int first_page = framebuffer & PAGE_ADDRESS;
    unsigned int last_byte = framebuffer + framebuffer_bytes - 1u;
    unsigned int last_page = last_byte & PAGE_ADDRESS;
    unsigned int first_directory = first_page >> 22;
    unsigned int last_directory = last_page >> 22;
    if(first_directory < IDENTITY_TABLES || last_directory >= 1023u ||
       last_directory - first_directory >= FRAMEBUFFER_TABLES)
        return 0;

    for(unsigned int i = 0; i < FRAMEBUFFER_TABLES; i++)
        for(unsigned int j = 0; j < 1024; j++)
            framebuffer_tables[i][j] = 0;

    for(unsigned int directory = first_directory; directory <= last_directory; directory++) {
        unsigned int table_index = directory - first_directory;
        page_directory[directory] =
            (unsigned int)framebuffer_tables[table_index] | PAGE_PRESENT | PAGE_WRITABLE;
    }
    for(unsigned int address = first_page; ; address += PAGE_SIZE) {
        unsigned int directory = address >> 22;
        unsigned int table_index = directory - first_directory;
        unsigned int page_index = (address >> 12) & 0x3FFu;
        framebuffer_tables[table_index][page_index] = address | PAGE_PRESENT | PAGE_WRITABLE;
        if(address == last_page) break;
    }

    asm volatile("" ::: "memory");
    unsigned int directory_address = (unsigned int)page_directory;
    asm volatile("mov %0, %%cr3" :: "r"(directory_address) : "memory");
    unsigned int cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u;
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    paging_active = 1;
    return 1;
}

int paging_is_active(void) {
    return paging_active;
}

unsigned int paging_directory_address(void) {
    return (unsigned int)page_directory;
}

int paging_create_user_space(unsigned int slot, unsigned int code_page, unsigned int stack_page) {
    if(slot >= USER_PROCESS_LIMIT || !paging_active ||
       (code_page & (PAGE_SIZE-1u)) || (stack_page & (PAGE_SIZE-1u)) ||
       code_page >= PAGING_IDENTITY_LIMIT || stack_page >= PAGING_IDENTITY_LIMIT ||
       code_page == stack_page)
        return 0;
    for(unsigned int i = 0; i < 1024; i++) {
        user_directories[slot][i] = page_directory[i];
        user_tables[slot][i] = 0;
    }
    user_tables[slot][0] = code_page | PAGE_PRESENT | PAGE_USER;
    user_tables[slot][2] = stack_page | PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
    user_directories[slot][USER_CODE_ADDRESS >> 22] =
        (unsigned int)user_tables[slot] | PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE;
    return 1;
}

int paging_activate_user_space(unsigned int slot) {
    if(slot >= USER_PROCESS_LIMIT || !paging_active) return 0;
    unsigned int address = (unsigned int)user_directories[slot];
    asm volatile("mov %0, %%cr3" :: "r"(address) : "memory");
    return 1;
}

void paging_activate_kernel_space(void) {
    unsigned int address = (unsigned int)page_directory;
    asm volatile("mov %0, %%cr3" :: "r"(address) : "memory");
}
