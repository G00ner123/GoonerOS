#include "kernel.h"

typedef struct {
    int active;
    int pid;
    int type;
    int state;
    unsigned int steps;
    unsigned int progress;
    unsigned int total;
    unsigned int crc;
} scheduler_task_t;

static scheduler_task_t tasks[SCHEDULER_MAX_TASKS];
static unsigned int next_pid = 2;
static int next_slot = 0;
static unsigned char checksum_data[FS_MAX_FILE_BYTES + 1];

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
        tasks[i].steps = 0;
        tasks[i].progress = 0;
        tasks[i].total = 0;
        tasks[i].crc = 0;
    }
    next_pid = 2;
    next_slot = 0;
}

static int scheduler_add_task(int type) {
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(tasks[i].active) continue;
        int pid = scheduler_allocate_pid();
        if(pid < 0) return -1;
        tasks[i].active = 1;
        tasks[i].pid = pid;
        tasks[i].type = type;
        tasks[i].state = SCHEDULER_STATE_RUNNABLE;
        tasks[i].steps = 0;
        tasks[i].progress = 0;
        tasks[i].total = 0;
        tasks[i].crc = 0xFFFFFFFFu;
        return tasks[i].pid;
    }
    return -1;
}

int scheduler_spawn_counter(void) {
    return scheduler_add_task(SCHEDULER_TASK_COUNTER);
}

int scheduler_spawn_checksum(const char* path) {
    if(!path || !path[0]) return -1;
    int free_slot = 0;
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++)
        if(!tasks[i].active) free_slot = 1;
        else if(tasks[i].type == SCHEDULER_TASK_CHECKSUM &&
                tasks[i].state == SCHEDULER_STATE_RUNNABLE) return -1;
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
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(tasks[i].active && tasks[i].pid == pid) {
            tasks[i].active = 0;
            tasks[i].pid = 0;
            tasks[i].steps = 0;
            tasks[i].progress = 0;
            tasks[i].total = 0;
            tasks[i].crc = 0;
            return 1;
        }
    }
    return 0;
}

void scheduler_run(void) {
    for(int scanned = 0; scanned < SCHEDULER_MAX_TASKS; scanned++) {
        int i = next_slot;
        next_slot = (next_slot + 1) % SCHEDULER_MAX_TASKS;
        if(tasks[i].active && tasks[i].state == SCHEDULER_STATE_RUNNABLE) {
            tasks[i].steps++;
            if(tasks[i].type == SCHEDULER_TASK_CHECKSUM) {
                unsigned int end = tasks[i].progress + 64;
                if(end > tasks[i].total) end = tasks[i].total;
                while(tasks[i].progress < end) {
                    tasks[i].crc ^= checksum_data[tasks[i].progress++];
                    for(int bit = 0; bit < 8; bit++)
                        tasks[i].crc = (tasks[i].crc >> 1) ^
                            (0xEDB88320u & (0u - (tasks[i].crc & 1u)));
                }
                if(tasks[i].progress >= tasks[i].total) {
                    tasks[i].crc = ~tasks[i].crc;
                    tasks[i].state = SCHEDULER_STATE_DONE;
                }
            }
            return;
        }
    }
}

int scheduler_task_count(void) {
    int count = 0;
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++)
        if(tasks[i].active) count++;
    return count;
}

int scheduler_get_task(int index, int* pid, unsigned int* steps) {
    int current = 0;
    if(!pid || !steps || index < 0) return 0;
    for(int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        if(!tasks[i].active) continue;
        if(current++ != index) continue;
        *pid = tasks[i].pid;
        *steps = tasks[i].steps;
        return 1;
    }
    return 0;
}

int scheduler_get_task_info(int index, int* pid, int* type, int* state,
                            unsigned int* steps, unsigned int* progress,
                            unsigned int* total, unsigned int* result) {
    int current = 0;
    if(!pid || !type || !state || !steps || !progress || !total || !result || index < 0)
        return 0;
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
        return 1;
    }
    return 0;
}
