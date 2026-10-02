#include "kernel.h"

#define USER_CODE_ADDRESS 0x40000000u
#define USER_STACK_ADDRESS 0x40001000u
#define USER_PAGE_SIZE 4096u
#define USER_PROCESS_LIMIT 2u
#define USER_KERNEL_STACK_SIZE 8192u

extern char _user_app_start;
extern char _user_app_end;
extern char stack_top;
extern void user_enter(unsigned int entry, unsigned int stack, unsigned int argument);
extern volatile unsigned int user_kernel_stack;
extern volatile unsigned int user_faulted;

static unsigned char user_kernel_stacks[USER_PROCESS_LIMIT][USER_KERNEL_STACK_SIZE]
    __attribute__((aligned(4096)));
static int current_user_pid;
static int user_process_active;

static int user_pointer_valid(unsigned int address, unsigned int length) {
    if(address < USER_CODE_ADDRESS || address >= USER_STACK_ADDRESS + USER_PAGE_SIZE) return 0;
    if(length > USER_STACK_ADDRESS + USER_PAGE_SIZE - address) return 0;
    if(address < USER_STACK_ADDRESS && length > USER_STACK_ADDRESS - address) return 0;
    return 1;
}

int user_syscall_dispatch(unsigned int* registers) {
    if(!registers || !user_process_active) return 1;
    unsigned int number = registers[7];
    if(number == 1) {
        unsigned int address = registers[4];
        unsigned int length = registers[6];
        if(length > 128 || !user_pointer_valid(address, length)) {
            registers[7] = 0xFFFFFFFFu;
            return 0;
        }
        const char* text = (const char*)address;
        for(unsigned int i = 0; i < length; i++) vga_putc(text[i]);
        registers[7] = length;
        return 0;
    }
    if(number == 2) return 1;
    if(number == 3) {
        registers[7] = (unsigned int)current_user_pid;
        return 0;
    }
    registers[7] = 0xFFFFFFFFu;
    return 0;
}

void user_fault_dispatch(unsigned int error, unsigned int eip, unsigned int cs,
                         unsigned int address) {
    if((cs & 3u) != 3u) {
        vga_print("Kernel exception: ");
        print_int((int)eip);
        vga_putc(' ');
        print_int((int)error);
        vga_putc(' ');
        print_int((int)address);
        vga_putc('\n');
        for(;;) asm volatile("hlt");
    }
    user_faulted = 1;
}

int user_process_run(int pid, int test_fault) {
    unsigned int slot;
    unsigned int app_length = (unsigned int)(&_user_app_end - &_user_app_start);
    unsigned char* code_page;
    unsigned char* stack_page;
    if(pid < 1 || (unsigned int)pid > USER_PROCESS_LIMIT ||
       app_length == 0 || app_length > USER_PAGE_SIZE || user_process_active)
        return 0;
    slot = (unsigned int)pid - 1u;
    code_page = (unsigned char*)page_alloc();
    stack_page = (unsigned char*)page_alloc();
    if(!code_page || !stack_page) {
        if(code_page) page_free(code_page);
        if(stack_page) page_free(stack_page);
        return 0;
    }
    for(unsigned int i = 0; i < USER_KERNEL_STACK_SIZE; i++)
        user_kernel_stacks[slot][i] = 0;
    for(unsigned int i = 0; i < app_length; i++)
        code_page[i] = (unsigned char)(&_user_app_start)[i];
    if(!paging_create_user_space(slot, (unsigned int)code_page, (unsigned int)stack_page)) {
        page_free(code_page);
        page_free(stack_page);
        return 0;
    }
    user_faulted = 0;
    current_user_pid = pid;
    user_process_active = 1;
    gdt_set_kernel_stack((unsigned int)&user_kernel_stacks[slot][USER_KERNEL_STACK_SIZE]);
    if(!paging_activate_user_space(slot)) {
        user_process_active = 0;
        paging_activate_kernel_space();
        gdt_set_kernel_stack((unsigned int)&stack_top);
        page_free(code_page);
        page_free(stack_page);
        return 0;
    }
    user_enter(USER_CODE_ADDRESS, USER_STACK_ADDRESS + USER_PAGE_SIZE - 16u,
               test_fault ? 1u : 0u);
    user_process_active = 0;
    paging_activate_kernel_space();
    gdt_set_kernel_stack((unsigned int)&stack_top);
    if(!page_free(code_page) || !page_free(stack_page)) {
        vga_print("Kernel page allocator release failure\n");
        for(;;) asm volatile("hlt");
    }
    return user_faulted ? -1 : 1;
}
