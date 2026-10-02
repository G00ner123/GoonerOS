#include "kernel.h"

#define USER_CODE_ADDRESS 0x40000000u
<<<<<<< HEAD
#define USER_STACK_ADDRESS 0x40002000u
#define USER_PAGE_SIZE 4096u
#define USER_KERNEL_STACK_SIZE 8192u
#define USER_ELF_MAX_SEGMENTS 8u

typedef struct {
    int active;
    int pid;
    int state;
    int wait_pid;
    int exit_status;
    unsigned int saved_esp;
    unsigned int steps;
    unsigned int wake_tick;
    unsigned char* code_page;
    unsigned char* stack_page;
} user_task_t;

typedef struct {
    unsigned char ident[16];
    unsigned short type;
    unsigned short machine;
    unsigned int version;
    unsigned int entry;
    unsigned int phoff;
    unsigned int shoff;
    unsigned int flags;
    unsigned short ehsize;
    unsigned short phentsize;
    unsigned short phnum;
    unsigned short shentsize;
    unsigned short shnum;
    unsigned short shstrndx;
} __attribute__((packed)) user_elf_header_t;

typedef struct {
    unsigned int type;
    unsigned int offset;
    unsigned int vaddr;
    unsigned int paddr;
    unsigned int filesz;
    unsigned int memsz;
    unsigned int flags;
    unsigned int align;
} __attribute__((packed)) user_elf_program_header_t;

typedef struct {
    unsigned int start;
    unsigned int end;
} user_elf_range_t;

typedef struct {
    int pid;
    int status;
} user_exit_record_t;
=======
#define USER_STACK_ADDRESS 0x40001000u
#define USER_PAGE_SIZE 4096u
#define USER_PROCESS_LIMIT 2u
#define USER_KERNEL_STACK_SIZE 8192u
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54

extern char _user_app_start;
extern char _user_app_end;
extern char stack_top;
extern void user_enter(unsigned int entry, unsigned int stack, unsigned int argument);
extern volatile unsigned int user_kernel_stack;
extern volatile unsigned int user_faulted;

static unsigned char user_kernel_stacks[USER_PROCESS_LIMIT][USER_KERNEL_STACK_SIZE]
    __attribute__((aligned(4096)));
<<<<<<< HEAD
static user_task_t user_tasks[USER_PROCESS_LIMIT];
static int current_user_pid;
static int current_user_slot = -1;
static int user_process_active;
static int next_user_pid = 100;
static unsigned int user_exit_record_next;
static user_exit_record_t user_exit_records[USER_PROCESS_LIMIT];
static unsigned char user_elf_file[FS_MAX_FILE_BYTES + 1] __attribute__((aligned(4)));

static unsigned int user_process_interrupts_save_disable(void) {
    unsigned int flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void user_process_interrupts_restore(unsigned int flags) {
    if(flags & 0x200u) asm volatile("sti" ::: "memory");
}

static int user_elf_load(const unsigned char* file, unsigned int length,
                         unsigned char* code_page, unsigned int* entry);

static int user_pointer_valid(unsigned int address, unsigned int length) {
    unsigned int code_end = USER_CODE_ADDRESS + USER_PAGE_SIZE;
    unsigned int stack_end = USER_STACK_ADDRESS + USER_PAGE_SIZE;
    if(address >= USER_CODE_ADDRESS && address < code_end)
        return length <= code_end - address;
    if(address >= USER_STACK_ADDRESS && address < stack_end)
        return length <= stack_end - address;
    return 0;
}

static void user_task_release(user_task_t* task) {
    if(task->active) {
        user_exit_record_t* record =
            &user_exit_records[user_exit_record_next++ % USER_PROCESS_LIMIT];
        record->pid = task->pid;
        record->status = task->exit_status;
    }
    int code_released = task->code_page ? page_free(task->code_page) : 1;
    int stack_released = task->stack_page ? page_free(task->stack_page) : 1;
    if(!code_released || !stack_released)
        kernel_panic("User process page release failure");
    task->active = 0;
    task->pid = 0;
    task->state = SCHEDULER_STATE_DONE;
    task->wait_pid = 0;
    task->exit_status = 0;
    task->saved_esp = 0;
    task->steps = 0;
    task->wake_tick = 0;
    task->code_page = 0;
    task->stack_page = 0;
}

static int user_allocate_pid(void) {
    for(int attempt = 0; attempt <= USER_PROCESS_LIMIT; attempt++) {
        if(next_user_pid < 100 || next_user_pid >= 0x7FFFFFFF) next_user_pid = 100;
        int candidate = next_user_pid++;
        int used = 0;
        for(int i = 0; i < USER_PROCESS_LIMIT; i++)
            if(user_tasks[i].active && user_tasks[i].pid == candidate) used = 1;
        if(!used) return candidate;
    }
    return -1;
}

static int user_task_prepare(int slot, int mode, const unsigned char* image,
                             unsigned int image_length, unsigned int entry) {
    unsigned int app_length = (unsigned int)(&_user_app_end - &_user_app_start);
    if(slot < 0 || slot >= USER_PROCESS_LIMIT ||
       ((!image || !image_length) && (!app_length || app_length > USER_PAGE_SIZE)))
        return 0;
    unsigned char* code_page = (unsigned char*)page_alloc();
    unsigned char* stack_page = (unsigned char*)page_alloc();
    if(!code_page || !stack_page) {
        if(code_page) page_free(code_page);
        if(stack_page) page_free(stack_page);
        return 0;
    }
    for(unsigned int i = 0; i < USER_KERNEL_STACK_SIZE; i++)
        user_kernel_stacks[slot][i] = 0;
    for(unsigned int i = 0; i < USER_PAGE_SIZE; i++) {
        code_page[i] = 0;
        stack_page[i] = 0;
    }
    if(image) {
        if(!user_elf_load(image, image_length, code_page, &entry)) {
            page_free(code_page);
            page_free(stack_page);
            return 0;
        }
    } else {
        for(unsigned int i = 0; i < app_length; i++)
            code_page[i] = (unsigned char)(&_user_app_start)[i];
        entry = USER_CODE_ADDRESS;
    }
    if(!paging_create_user_space((unsigned int)slot,
                                 (unsigned int)code_page,
                                 (unsigned int)stack_page)) {
        page_free(code_page);
        page_free(stack_page);
        return 0;
    }
    int pid = user_allocate_pid();
    if(pid < 0) {
        page_free(code_page);
        page_free(stack_page);
        return 0;
    }
    user_task_t* task = &user_tasks[slot];
    unsigned int* top =
        (unsigned int*)&user_kernel_stacks[slot][USER_KERNEL_STACK_SIZE];
    unsigned int* frame = top-15;
    for(int i = 0; i < 15; i++) frame[i] = 0;
    frame[7] = (unsigned int)mode;
    frame[8] = 32;
    frame[10] = entry;
    frame[11] = 0x1B;
    frame[12] = 0x202;
    frame[13] = USER_STACK_ADDRESS + USER_PAGE_SIZE - 16u;
    frame[14] = 0x23;
    task->pid = pid;
    task->state = SCHEDULER_STATE_RUNNABLE;
    task->wait_pid = 0;
    task->exit_status = 0;
    task->saved_esp = (unsigned int)frame;
    task->steps = 0;
    task->wake_tick = 0;
    task->code_page = code_page;
    task->stack_page = stack_page;
    task->active = 1;
    return 1;
}

void user_process_init(void) {
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        user_tasks[i].active = 0;
        user_tasks[i].pid = 0;
        user_tasks[i].state = SCHEDULER_STATE_DONE;
        user_tasks[i].wait_pid = 0;
        user_tasks[i].exit_status = 0;
        user_tasks[i].saved_esp = 0;
        user_tasks[i].steps = 0;
        user_tasks[i].wake_tick = 0;
        user_tasks[i].code_page = 0;
        user_tasks[i].stack_page = 0;
    }
    current_user_pid = 0;
    current_user_slot = -1;
    user_process_active = 0;
    next_user_pid = 100;
    user_exit_record_next = 0;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        user_exit_records[i].pid = 0;
        user_exit_records[i].status = 0;
    }
}

int user_process_spawn_async(int mode) {
    if(mode < 0 || mode > 5) return -1;
    if(user_process_active) return -1;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        if(user_tasks[i].active) continue;
        return user_task_prepare(i, mode, 0, 0, 0) ? user_tasks[i].pid : -1;
    }
    return -1;
}

static int user_elf_load(const unsigned char* file, unsigned int length,
                         unsigned char* code_page, unsigned int* entry) {
    if(!file || !code_page || !entry || length < sizeof(user_elf_header_t)) return 0;
    const user_elf_header_t* header = (const user_elf_header_t*)file;
    if(header->ident[0] != 0x7F || header->ident[1] != 'E' ||
       header->ident[2] != 'L' || header->ident[3] != 'F' ||
       header->ident[4] != 1 || header->ident[5] != 1 ||
       header->ident[6] != 1 || header->type != 2 ||
       header->machine != 3 || header->version != 1 ||
       header->ehsize != sizeof(user_elf_header_t) ||
       header->phentsize != sizeof(user_elf_program_header_t) ||
       !header->phnum || header->phnum > USER_ELF_MAX_SEGMENTS ||
       header->phoff < header->ehsize || header->phoff > length ||
       header->phnum > (length - header->phoff) / sizeof(user_elf_program_header_t))
        return 0;

    user_elf_range_t ranges[USER_ELF_MAX_SEGMENTS];
    unsigned int range_count = 0;
    int entry_executable = 0;
    for(unsigned int i = 0; i < header->phnum; i++) {
        const user_elf_program_header_t* program =
            (const user_elf_program_header_t*)(file + header->phoff +
                                               i * sizeof(user_elf_program_header_t));
        if(program->type == 2 || program->type == 3 || program->type == 7)
            return 0;
        if(program->type != 1) continue;
        if(!program->memsz || program->filesz > program->memsz ||
           (program->flags & 2u) ||
           program->offset > length || program->filesz > length - program->offset ||
           program->vaddr < USER_CODE_ADDRESS ||
           program->memsz > USER_PAGE_SIZE ||
           program->vaddr - USER_CODE_ADDRESS > USER_PAGE_SIZE - program->memsz ||
           (program->align > 1 &&
            ((program->align & (program->align - 1u)) ||
             program->vaddr % program->align != program->offset % program->align)))
            return 0;
        unsigned int start = program->vaddr;
        unsigned int end = start + program->memsz;
        for(unsigned int j = 0; j < range_count; j++)
            if(start < ranges[j].end && end > ranges[j].start) return 0;
        ranges[range_count].start = start;
        ranges[range_count++].end = end;
        if((program->flags & 1u) && header->entry >= start && header->entry < end)
            entry_executable = 1;
        unsigned int destination = start - USER_CODE_ADDRESS;
        for(unsigned int j = 0; j < program->filesz; j++)
            code_page[destination + j] = file[program->offset + j];
    }
    if(!range_count || !entry_executable) return 0;
    *entry = header->entry;
    return 1;
}

int user_process_spawn_elf(const char* path) {
    if(!path || !path[0] || user_process_active) return -1;
    struct fs_entry* entry = fs_find(path);
    if(!entry || fs_is_directory(entry) || !entry->size ||
       entry->size > sizeof(user_elf_file)) return -1;
    int file_length = fs_read(path, (char*)user_elf_file, sizeof(user_elf_file));
    if(file_length <= 0 || (unsigned int)file_length != entry->size) return -1;
    int free_slot = -1;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++)
        if(!user_tasks[i].active) { free_slot = i; break; }
    if(free_slot < 0) return -1;

    return user_task_prepare(free_slot, 0, user_elf_file,
                             (unsigned int)file_length, 0)
        ? user_tasks[free_slot].pid : -1;
}

int user_process_task_count(void) {
    unsigned int flags = user_process_interrupts_save_disable();
    int count = 0;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++)
        if(user_tasks[i].active) count++;
    user_process_interrupts_restore(flags);
    return count;
}

int user_process_get_info(int index, int* pid, int* state, unsigned int* steps) {
    unsigned int flags = user_process_interrupts_save_disable();
    if(index < 0 || !pid || !state || !steps) {
        user_process_interrupts_restore(flags);
        return 0;
    }
    int current = 0;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        if(!user_tasks[i].active) continue;
        if(current++ != index) continue;
        *pid = user_tasks[i].pid;
        *state = user_tasks[i].state;
        *steps = user_tasks[i].steps;
        user_process_interrupts_restore(flags);
        return 1;
    }
    user_process_interrupts_restore(flags);
    return 0;
}

int user_process_kill(int pid) {
    unsigned int flags = user_process_interrupts_save_disable();
    int slot = -1;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++)
        if(user_tasks[i].active && user_tasks[i].pid == pid) slot = i;
    if(slot < 0) {
        user_process_interrupts_restore(flags);
        return 0;
    }
    user_tasks[slot].exit_status = -1;
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        if(user_tasks[i].active && user_tasks[i].state == SCHEDULER_STATE_WAITING &&
           user_tasks[i].wait_pid == pid) {
            if(user_tasks[i].saved_esp)
                ((unsigned int*)user_tasks[i].saved_esp)[7] = 0xFFFFFFFFu;
            user_tasks[i].wait_pid = 0;
            user_tasks[i].state = SCHEDULER_STATE_RUNNABLE;
        }
    }
    user_task_release(&user_tasks[slot]);
    user_process_interrupts_restore(flags);
    return 1;
}

int user_process_current_async(void) {
    return current_user_slot >= 0 && current_user_slot < USER_PROCESS_LIMIT &&
           user_tasks[current_user_slot].active;
}

int user_process_current_pid(void) {
    if(user_process_current_async()) return user_tasks[current_user_slot].pid;
    return current_user_pid;
}

void user_process_async_exit(int status) {
    if(user_process_current_async()) {
        user_tasks[current_user_slot].exit_status = status;
        user_tasks[current_user_slot].state = SCHEDULER_STATE_DONE;
    }
}

void user_process_wake_sleepers(void) {
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        user_task_t* task = &user_tasks[i];
        if(task->active && task->state == SCHEDULER_STATE_SLEEPING &&
           (int)(ticks - task->wake_tick) >= 0)
            task->state = SCHEDULER_STATE_RUNNABLE;
    }
}

void user_process_save_stack(int index, unsigned int stack_pointer) {
    if(index >= 0 && index < USER_PROCESS_LIMIT && user_tasks[index].active) {
        user_tasks[index].saved_esp = stack_pointer;
        user_tasks[index].steps++;
    }
    current_user_slot = -1;
}

int user_process_runnable(int index) {
    return index >= 0 && index < USER_PROCESS_LIMIT &&
           user_tasks[index].active &&
           user_tasks[index].state == SCHEDULER_STATE_RUNNABLE;
}

unsigned int user_process_saved_stack(int index) {
    if(!user_process_runnable(index)) return 0;
    return user_tasks[index].saved_esp;
}

int user_process_activate(int index) {
    if(!user_process_runnable(index) || !paging_activate_user_space((unsigned int)index))
        return 0;
    current_user_slot = index;
    gdt_set_kernel_stack((unsigned int)&user_kernel_stacks[index][USER_KERNEL_STACK_SIZE]);
    return 1;
}

void user_process_reap_done(void) {
    for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
        if(!user_tasks[i].active || user_tasks[i].state != SCHEDULER_STATE_DONE)
            continue;
        int pid = user_tasks[i].pid;
        int status = user_tasks[i].exit_status;
        for(int waiter = 0; waiter < USER_PROCESS_LIMIT; waiter++) {
            if(!user_tasks[waiter].active ||
               user_tasks[waiter].state != SCHEDULER_STATE_WAITING ||
               user_tasks[waiter].wait_pid != pid)
                continue;
            if(user_tasks[waiter].saved_esp)
                ((unsigned int*)user_tasks[waiter].saved_esp)[7] = (unsigned int)status;
            user_tasks[waiter].wait_pid = 0;
            user_tasks[waiter].state = SCHEDULER_STATE_RUNNABLE;
        }
        user_task_release(&user_tasks[i]);
    }
}

int user_syscall_dispatch(unsigned int* registers) {
    if(!registers || (!user_process_active && !user_process_current_async())) return 1;
=======
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
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
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
<<<<<<< HEAD
    if(number == 2) {
        if(user_process_current_async()) {
            user_process_async_exit((int)registers[4]);
            return 2;
        }
        return 1;
    }
    if(number == 3) {
        registers[7] = (unsigned int)user_process_current_pid();
        return 0;
    }
    if(number == 4) {
        if(!user_process_current_async()) {
            registers[7] = 0xFFFFFFFFu;
            return 0;
        }
        registers[7] = 0;
        return 2;
    }
    if(number == 5) {
        if(!user_process_current_async()) {
            registers[7] = 0xFFFFFFFFu;
            return 0;
        }
        unsigned int delay = registers[4];
        if(delay > 0x7FFFFFFFu) {
            registers[7] = 0xFFFFFFFFu;
            return 0;
        }
        if(delay) {
            user_task_t* task = &user_tasks[current_user_slot];
            task->wake_tick = ticks + delay;
            task->state = SCHEDULER_STATE_SLEEPING;
        }
        registers[7] = 0;
        return 2;
    }
    if(number == 6) {
        if(!user_process_current_async()) {
            registers[7] = 0xFFFFFFFFu;
            return 0;
        }
        int target_pid = (int)registers[4];
        if(target_pid == user_tasks[current_user_slot].pid) {
            registers[7] = 0xFFFFFFFFu;
            return 0;
        }
        for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
            user_task_t* target = &user_tasks[i];
            if(!target->active || target->pid != target_pid) continue;
            if(target->state == SCHEDULER_STATE_DONE) {
                registers[7] = (unsigned int)target->exit_status;
                return 0;
            }
            user_tasks[current_user_slot].wait_pid = target_pid;
            user_tasks[current_user_slot].state = SCHEDULER_STATE_WAITING;
            return 2;
        }
        for(int i = 0; i < USER_PROCESS_LIMIT; i++) {
            if(user_exit_records[i].pid != target_pid) continue;
            registers[7] = (unsigned int)user_exit_records[i].status;
            return 0;
        }
        registers[7] = 0xFFFFFFFFu;
=======
    if(number == 2) return 1;
    if(number == 3) {
        registers[7] = (unsigned int)current_user_pid;
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
        return 0;
    }
    registers[7] = 0xFFFFFFFFu;
    return 0;
}

<<<<<<< HEAD
int user_fault_dispatch(unsigned int error, unsigned int eip, unsigned int cs,
                        unsigned int address) {
=======
void user_fault_dispatch(unsigned int error, unsigned int eip, unsigned int cs,
                         unsigned int address) {
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
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
<<<<<<< HEAD
    if(user_process_current_async()) {
        user_process_async_exit(128);
        return 2;
    }
    user_faulted = 1;
    return 1;
=======
    user_faulted = 1;
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
}

int user_process_run(int pid, int test_fault) {
    unsigned int slot;
    unsigned int app_length = (unsigned int)(&_user_app_end - &_user_app_start);
    unsigned char* code_page;
    unsigned char* stack_page;
    if(pid < 1 || (unsigned int)pid > USER_PROCESS_LIMIT ||
<<<<<<< HEAD
       app_length == 0 || app_length > USER_PAGE_SIZE ||
       user_process_active || user_process_task_count())
=======
       app_length == 0 || app_length > USER_PAGE_SIZE || user_process_active)
>>>>>>> 409f10ca7eb9b89ecee2ad93002bb7b6b9e15e54
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
