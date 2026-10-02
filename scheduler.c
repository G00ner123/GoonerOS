#include "kernel.h"

typedef struct {
    int active;
    int pid;
    int type;
    int state;
    unsigned int saved_esp;
    unsigned int steps;
    unsigned int progress;
    unsigned int total;
    unsigned int crc;
} scheduler_task_t;

static scheduler_task_t tasks[SCHEDULER_MAX_TASKS];
static unsigned int next_pid = 2;
static int next_slot = 0;
static int current_task_slot = -1;
static int current_user_slot = -1;
static unsigned int main_stack_pointer;
static unsigned char checksum_data[FS_MAX_FILE_BYTES + 1];
static unsigned char task_stacks[SCHEDULER_MAX_TASKS][8192] __attribute__((aligned(16)));
extern char stack_top;

static unsigned int scheduler_interrupts_save_disable(void) {
    unsigned int flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void scheduler_interrupts_restore(unsigned int flags) {
    if(flags & 0x200u) asm volatile("sti" ::: "memory");
}

static void scheduler_task_exit(void) {
    for(;;) asm volatile("hlt");
}

static void scheduler_task_bootstrap(void) {
    int slot = current_task_slot;
    if(slot < 0 || slot >= SCHEDULER_MAX_TASKS) scheduler_task_exit();
    scheduler_task_t* task = &tasks[slot];
    if(task->type == SCHEDULER_TASK_COUNTER) {
        while(task->active && task->state == SCHEDULER_STATE_RUNNABLE)
            task->steps++;
    } else {
        if(task->total == 0) {
            task->crc = ~task->crc;
            task->state = SCHEDULER_STATE_DONE;
        }
        while(task->active && task->state == SCHEDULER_STATE_RUNNABLE) {
            unsigned int end = task->progress + 64;
            if(end > task->total) end = task->total;
            while(task->progress < end) {
                task->crc ^= checksum_data[task->progress++];
                for(int bit = 0; bit < 8; bit++)
                    task->crc = (task->crc >> 1) ^
                        (0xEDB88320u & (0u - (task->crc & 1u)));
            }
            task->steps++;
            if(task->progress >= task->total) {
                task->crc = ~task->crc;
                task->state = SCHEDULER_STATE_DONE;
            }
        }
    }
    scheduler_task_exit();
}

static int scheduler_allocate_pid(void) {
    for(int attempt = 0; attempt <= SCHEDULER_MAX_TASKS; attempt++) {
        if(next_pid < 2 || next_pid > 0x7FFFFFFFu) next_pid = 2;
        unsigned int candidate = next_pid++;
        int used = 0;
        for(int i = 0; i < SCHEDULER_MAX_TASKS; i++)
            if(tasks[i].active && tasks[i].pid == (int)candidate) used = 1;
        if(!used) return (int)candidate;
    }
    return -1;
}

void scheduler_init(void) {
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        tasks[i].active = 0;
        tasks[i].pid = 0;
        tasks[i].type = SCHEDULER_TASK_COUNTER;
        tasks[i].state = SCHEDULER_STATE_DONE;
        tasks[i].saved_esp = 0;
        tasks[i].steps = 0;
        tasks[i].progress = 0;
        tasks[i].total = 0;
        tasks[i].crc = 0;
    }
    next_pid = 2;
    next_slot = 0;
    current_task_slot = -1;
    current_user_slot = -1;
    main_stack_pointer = 0;
    user_process_init();
}

static int scheduler_add_task(int type) {
    unsigned int flags = scheduler_interrupts_save_disable();
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(tasks[i].active) continue;
        int pid = scheduler_allocate_pid();
        if(pid < 0) {
            scheduler_interrupts_restore(flags);
            return -1;
        }
        tasks[i].pid = pid;
        tasks[i].type = type;
        tasks[i].state = SCHEDULER_STATE_RUNNABLE;
        unsigned int* top = (unsigned int*)&task_stacks[i][sizeof(task_stacks[i])];
        unsigned int* frame = top-14;
        for(int j = 0; j < 14; j++) frame[j] = 0;
        frame[3] = (unsigned int)top;
        frame[8] = 32;
        frame[10] = (unsigned int)scheduler_task_bootstrap;
        frame[11] = 0x08;
        frame[12] = 0x202;
        frame[13] = (unsigned int)scheduler_task_exit;
        tasks[i].saved_esp = (unsigned int)frame;
        tasks[i].steps = 0;
        tasks[i].progress = 0;
        tasks[i].total = 0;
        tasks[i].crc = 0xFFFFFFFFu;
        tasks[i].active = 1;
        scheduler_interrupts_restore(flags);
        return tasks[i].pid;
    }
    scheduler_interrupts_restore(flags);
    return -1;
}

int scheduler_spawn_counter(void) {
    return scheduler_add_task(SCHEDULER_TASK_COUNTER);
}

int scheduler_spawn_checksum(const char* path) {
    if(!path || !path[0]) return -1;
    int free_slot = 0;
    unsigned int flags = scheduler_interrupts_save_disable();
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(!tasks[i].active) free_slot = 1;
        else if(tasks[i].type == SCHEDULER_TASK_CHECKSUM &&
                tasks[i].state == SCHEDULER_STATE_RUNNABLE) {
            scheduler_interrupts_restore(flags);
            return -1;
        }
    }
    scheduler_interrupts_restore(flags);
    if(!free_slot) return -1;
    struct fs_entry* entry = fs_find(path);
    if(!entry || fs_is_directory(entry) || entry->size > FS_MAX_FILE_BYTES) return -1;
    int size = fs_read(path, (char*)checksum_data, FS_MAX_FILE_BYTES + 1);
    if(size < 0 || (unsigned int)size != entry->size) return -1;
    int pid = scheduler_add_task(SCHEDULER_TASK_CHECKSUM);
    if(pid < 0) return -1;
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(tasks[i].active && tasks[i].pid == pid) {
            tasks[i].total = (unsigned int)size;
            return pid;
        }
    }
    return -1;
}

int scheduler_kill(int pid) {
    if(pid < 2) return 0;
    if(pid >= 100) return user_process_kill(pid);
    unsigned int flags = scheduler_interrupts_save_disable();
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(tasks[i].active && tasks[i].pid == pid) {
            tasks[i].pid = 0;
            tasks[i].steps = 0;
            tasks[i].progress = 0;
            tasks[i].total = 0;
            tasks[i].crc = 0;
            tasks[i].active = 0;
            scheduler_interrupts_restore(flags);
            return 1;
        }
    }
    scheduler_interrupts_restore(flags);
    return 0;
}

unsigned int scheduler_timer_switch(unsigned int stack_pointer) {
    unsigned int* frame = (unsigned int*)stack_pointer;
    int from_user = (frame[11] & 3u) == 3u;
    if(from_user && !user_process_current_async()) return stack_pointer;
    if(from_user) {
        user_process_save_stack(current_user_slot, stack_pointer);
        current_user_slot = -1;
        user_process_reap_done();
    } else if(current_task_slot >= 0)
        tasks[current_task_slot].saved_esp = stack_pointer;
    else
        main_stack_pointer = stack_pointer;
    user_process_wake_sleepers();
    int context_count = SCHEDULER_MAX_TASKS + 1 + USER_PROCESS_LIMIT;
    for(int scanned = 0; scanned < context_count; scanned++) {
        int slot = next_slot;
        next_slot = (next_slot+1)%context_count;
        if(slot > SCHEDULER_MAX_TASKS) {
            int user_slot = slot-SCHEDULER_MAX_TASKS-1;
            unsigned int user_stack = user_process_saved_stack(user_slot);
            if(!user_stack) continue;
            if(!user_process_activate(user_slot))
                kernel_panic("Unable to activate scheduled user process");
            current_task_slot = -1;
            current_user_slot = user_slot;
            return user_stack;
        }
        if(slot == SCHEDULER_MAX_TASKS) {
            if(!main_stack_pointer) continue;
            current_task_slot = -1;
            current_user_slot = -1;
            if(from_user) {
                paging_activate_kernel_space();
                gdt_set_kernel_stack((unsigned int)&stack_top);
            }
            return main_stack_pointer;
        }
        if(tasks[slot].active && tasks[slot].state == SCHEDULER_STATE_RUNNABLE) {
            current_task_slot = slot;
            current_user_slot = -1;
            if(from_user) {
                paging_activate_kernel_space();
                gdt_set_kernel_stack((unsigned int)&stack_top);
            }
            return tasks[slot].saved_esp;
        }
    }
    current_task_slot = -1;
    current_user_slot = -1;
    if(from_user) {
        paging_activate_kernel_space();
        gdt_set_kernel_stack((unsigned int)&stack_top);
    }
    return main_stack_pointer ? main_stack_pointer : stack_pointer;
}

int scheduler_task_count(void) {
    unsigned int flags = scheduler_interrupts_save_disable();
    int count = 0;
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++)
        if(tasks[i].active) count++;
    count += user_process_task_count();
    scheduler_interrupts_restore(flags);
    return count;
}

int scheduler_get_task(int index, int* pid, unsigned int* steps) {
    unsigned int flags = scheduler_interrupts_save_disable();
    int current = 0;
    if(!pid || !steps || index < 0) {
        scheduler_interrupts_restore(flags);
        return 0;
    }
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(!tasks[i].active) continue;
        if(current++ != index) continue;
        *pid = tasks[i].pid;
        *steps = tasks[i].steps;
        scheduler_interrupts_restore(flags);
        return 1;
    }
    scheduler_interrupts_restore(flags);
    return 0;
}

int scheduler_get_task_info(int index, int* pid, int* type, int* state,
                            unsigned int* steps, unsigned int* progress,
                            unsigned int* total, unsigned int* result) {
    unsigned int flags = scheduler_interrupts_save_disable();
    int current = 0;
    if(!pid || !type || !state || !steps || !progress || !total || !result || index < 0) {
        scheduler_interrupts_restore(flags);
        return 0;
    }
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(!tasks[i].active) continue;
        if(current++ != index) continue;
        *pid = tasks[i].pid;
        *type = tasks[i].type;
        *state = tasks[i].state;
        *steps = tasks[i].steps;
        *progress = tasks[i].progress;
        *total = tasks[i].total;
        *result = tasks[i].state == SCHEDULER_STATE_DONE ? tasks[i].crc : 0;
        scheduler_interrupts_restore(flags);
        return 1;
    }
    int user_index = index-current;
    if(user_process_get_info(user_index, pid, state, steps)) {
        *type = SCHEDULER_TASK_USER;
        *progress = 0;
        *total = 0;
        *result = 0;
        scheduler_interrupts_restore(flags);
        return 1;
    }
    scheduler_interrupts_restore(flags);
    return 0;
}
