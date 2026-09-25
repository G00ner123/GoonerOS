#include "kernel.h"
#include "io.h"

struct fs_entry fs_table[FS_MAX_FILES];
struct fs_superblock fs_sb;
static unsigned char fs_iobuf[FS_MAX_FILE_BYTES];
static struct fs_entry fs_legacy_table[FS_MAX_FILES];

static int fs_name_valid(const char* name) {
    if(!name || !name[0] || strlen(name) >= FS_NAME_LEN) return 0;
    int component_start = 1;
    int component_len = 0;
    for(int i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if(c == '/') {
            if(component_len == 0) return 0;
            if(component_len == 1 && name[i-1] == '.') return 0;
            if(component_len == 2 && name[i-1] == '.' && name[i-2] == '.') return 0;
            component_start = 1;
            component_len = 0;
        } else {
            if(c < 32 || c == '\\' || c == ':' ) return 0;
            if(component_start && c == ' ') return 0;
            component_start = 0;
            component_len++;
        }
    }
    if(component_len == 0) return 0;
    if(component_len == 1 && name[strlen(name)-1] == '.') return 0;
    if(component_len == 2 && name[strlen(name)-1] == '.' && name[strlen(name)-2] == '.') return 0;
    return 1;
}

int fs_is_directory(const struct fs_entry* entry) {
    return entry && entry->used && entry->reserved[0] == FS_ENTRY_DIRECTORY_MARK;
}

static int fs_has_slash(const char* path) {
    for(int i = 0; path[i]; i++)
        if(path[i] == '/') return 1;
    return 0;
}

static void fs_reset(void) {
    fs_sb.magic = FS_MAGIC;
    fs_sb.file_count = 0;
    fs_sb.next_free_lba = 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        fs_table[i].used = 0;
        fs_table[i].name[0] = 0;
        fs_table[i].size = 0;
        fs_table[i].start_lba = 0;
        for(int j = 0; j < sizeof(fs_table[i].reserved); j++)
            fs_table[i].reserved[j] = 0;
    }
}

static void fs_clear_entry(struct fs_entry* entry) {
    entry->used = 0;
    for(int i = 0; i < FS_NAME_LEN; i++)
        entry->name[i] = 0;
    entry->size = 0;
    entry->start_lba = 0;
    for(int i = 0; i < sizeof(entry->reserved); i++)
        entry->reserved[i] = 0;
}

static int fs_entry_valid(const struct fs_entry* e) {
    if(!e->used || e->name[FS_NAME_LEN - 1] != 0 ||
       !fs_name_valid(e->name) ||
       e->start_lba < FS_DATA_START_LBA ||
       e->start_lba > FS_DISK_END_LBA - FS_MAX_FILE_SECTORS)
        return 0;
    if(fs_is_directory(e)) return e->size == 0;
    return e->size <= FS_MAX_FILE_BYTES;
}

static int fs_block_free(unsigned int start_lba) {
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_entry_valid(&fs_table[i]) &&
           start_lba < fs_table[i].start_lba + FS_MAX_FILE_SECTORS &&
           start_lba + FS_MAX_FILE_SECTORS > fs_table[i].start_lba)
            return 0;
    return 1;
}

static unsigned int fs_allocate_block(void) {
    for(unsigned int lba = FS_DATA_START_LBA;
        lba + FS_MAX_FILE_SECTORS <= FS_DISK_END_LBA;
        lba += FS_MAX_FILE_SECTORS)
        if(fs_block_free(lba)) return lba;
    return 0;
}

static int fs_parent_exists(const char* path) {
    char parent[FS_NAME_LEN];
    int len = strlen(path), slash = -1;
    for(int i = 0; i < len; i++) if(path[i] == '/') slash = i;
    if(slash < 0) return 1;
    if(slash == 0) return 0;
    for(int i = 0; i < slash; i++) parent[i] = path[i];
    parent[slash] = 0;
    return fs_is_directory(fs_find(parent));
}

static int fs_legacy_entry_valid(const struct fs_entry* e) {
    if(!e->used || e->name[FS_NAME_LEN-1] != 0 || !fs_name_valid(e->name) ||
       e->start_lba < FS_LEGACY_DATA_START_LBA ||
       e->start_lba > FS_DISK_END_LBA - FS_MAX_FILE_SECTORS)
        return 0;
    if(fs_is_directory(e)) return e->size == 0;
    return e->size <= FS_MAX_FILE_BYTES;
}

static int fs_legacy_parent_exists(const char* path) {
    int slash = -1;
    for(int i = 0; path[i]; i++) if(path[i] == '/') slash = i;
    if(slash < 0) return 1;
    if(slash == 0) return 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_entry* parent = &fs_legacy_table[i];
        if(!fs_legacy_entry_valid(parent) || !fs_is_directory(parent)) continue;
        if((int)strlen(parent->name) != slash || strncmp(parent->name, path, slash) != 0) continue;
        return 1;
    }
    return 0;
}

static int fs_legacy_sources_valid(void) {
    for(int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_entry* entry = &fs_legacy_table[i];
        if(!entry->used) continue;
        if(!fs_legacy_entry_valid(entry) || !fs_legacy_parent_exists(entry->name)) return 0;
        for(int j = 0; j < i; j++) {
            struct fs_entry* prior = &fs_legacy_table[j];
            if(!fs_legacy_entry_valid(prior)) continue;
            if(strcmp(entry->name, prior->name) == 0) return 0;
            if(entry->start_lba < prior->start_lba + FS_MAX_FILE_SECTORS &&
               entry->start_lba + FS_MAX_FILE_SECTORS > prior->start_lba) return 0;
        }
    }
    return 1;
}

static int fs_legacy_destination_block(void) {
    for(unsigned int lba = FS_DATA_START_LBA;
        lba + FS_MAX_FILE_SECTORS <= FS_DISK_END_LBA;
        lba += FS_MAX_FILE_SECTORS) {
        if(!fs_block_free(lba)) continue;
        int overlaps_source = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            struct fs_entry* source = &fs_legacy_table[i];
            if(!source->used) continue;
            if(lba < source->start_lba + FS_MAX_FILE_SECTORS &&
               lba + FS_MAX_FILE_SECTORS > source->start_lba) {
                overlaps_source = 1;
                break;
            }
        }
        if(!overlaps_source) return (int)lba;
    }
    return 0;
}

static int fs_migrate_legacy(void) {
    struct fs_superblock old_super;
    if(!ata_rw_sectors(FS_LEGACY_SUPERBLOCK_LBA, 1, (unsigned short*)&old_super, 0) ||
       old_super.magic != FS_MAGIC ||
       !ata_rw_sectors(FS_LEGACY_TABLE_LBA, FS_TABLE_SECTORS,
                       (unsigned short*)fs_legacy_table, 0) ||
       !fs_legacy_sources_valid())
        return 0;

    fs_reset();
    for(int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_entry* source = &fs_legacy_table[i];
        if(!source->used) continue;
        int destination = fs_legacy_destination_block();
        if(!destination) { fs_reset(); return 0; }
        fs_table[i] = *source;
        fs_table[i].start_lba = (unsigned int)destination;
        if(!fs_is_directory(source)) {
            if(!ata_rw_sectors(source->start_lba, FS_MAX_FILE_SECTORS,
                               (unsigned short*)fs_iobuf, 0) ||
               !ata_rw_sectors((unsigned int)destination, FS_MAX_FILE_SECTORS,
                               (unsigned short*)fs_iobuf, 1)) {
                fs_reset();
                return 0;
            }
        }
        fs_sb.file_count++;
    }
    fs_sb.magic = FS_MAGIC;
    if(!fs_save()) { fs_reset(); return 0; }
    return 1;
}

static int fs_create_entry(const char* name, int directory) {
    if(!fs_name_valid(name) || fs_find(name) || !fs_parent_exists(name)) return 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(fs_table[i].used) continue;
        struct fs_entry previous = fs_table[i];
        unsigned int previous_count = fs_sb.file_count;
        unsigned int block = fs_allocate_block();
        if(!block) return 0;
        for(int j = 0; j < FS_NAME_LEN; j++) fs_table[i].name[j] = 0;
        int j = 0;
        while(name[j]) { fs_table[i].name[j] = name[j]; j++; }
        fs_table[i].name[j] = 0;
        fs_table[i].size = 0;
        fs_table[i].start_lba = block;
        fs_table[i].used = 1;
        for(int k = 0; k < sizeof(fs_table[i].reserved); k++)
            fs_table[i].reserved[k] = 0;
        if(directory) fs_table[i].reserved[0] = FS_ENTRY_DIRECTORY_MARK;
        fs_sb.file_count++;
        if(!fs_save()) {
            fs_table[i] = previous;
            fs_sb.file_count = previous_count;
            fs_save();
            return 0;
        }
        return 1;
    }
    return 0;
}

int fs_save(void) {
    fs_sb.next_free_lba = 0;
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_entry_valid(&fs_table[i]) &&
           fs_table[i].start_lba + FS_MAX_FILE_SECTORS > fs_sb.next_free_lba)
            fs_sb.next_free_lba = fs_table[i].start_lba + FS_MAX_FILE_SECTORS;
    if(!ata_rw_sectors(FS_TABLE_LBA, FS_TABLE_SECTORS, (unsigned short*)fs_table, 1))
        return 0;
    return ata_rw_sectors(FS_SUPERBLOCK_LBA, 1, (unsigned short*)&fs_sb, 1);
}
void fs_load(void) {
    int ok = ata_rw_sectors(FS_SUPERBLOCK_LBA, 1, (unsigned short*)&fs_sb, 0);
    ok = ok && ata_rw_sectors(FS_TABLE_LBA, FS_TABLE_SECTORS, (unsigned short*)fs_table, 0);
    if(!ok) {
        // Platte antwortet nicht (z.B. kein/falsch konfiguriertes Laufwerk in QEMU) ->
        // ohne persistentes Dateisystem weiterstarten statt den Kernel haengen zu lassen
        fs_reset();
        return;
    }
    if(fs_sb.magic != FS_MAGIC) {
        if(fs_migrate_legacy()) return;
        fs_reset();
        fs_save();
        return;
    }
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_table[i].used && !fs_entry_valid(&fs_table[i]))
            fs_clear_entry(&fs_table[i]);

    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(!fs_entry_valid(&fs_table[i])) continue;
        for(int j = 0; j < i; j++) {
            if(!fs_entry_valid(&fs_table[j])) continue;
            int duplicate = strcmp(fs_table[i].name, fs_table[j].name) == 0;
            int overlap = fs_table[i].start_lba <
                              fs_table[j].start_lba + FS_MAX_FILE_SECTORS &&
                          fs_table[i].start_lba + FS_MAX_FILE_SECTORS >
                              fs_table[j].start_lba;
            if(duplicate || overlap) {
                fs_clear_entry(&fs_table[i]);
                break;
            }
        }
    }
    int removed_parent;
    do {
        removed_parent = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) {
            if(fs_entry_valid(&fs_table[i]) &&
               !fs_parent_exists(fs_table[i].name)) {
                fs_clear_entry(&fs_table[i]);
                removed_parent = 1;
            }
        }
    } while(removed_parent);
    fs_sb.file_count = 0;
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_entry_valid(&fs_table[i])) fs_sb.file_count++;
    fs_save();
}

struct fs_entry* fs_find(const char* name) {
    if(!name) return 0;
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_entry_valid(&fs_table[i]) && strcmp(fs_table[i].name, name) == 0) return &fs_table[i];
    return 0;
}

int fs_is_direct_child(const struct fs_entry* entry, const char* parent) {
    if(!entry || !fs_entry_valid(entry) || !parent) return 0;
    int parent_len = strlen(parent);
    const char* child = entry->name;
    if(parent_len == 0) return !fs_has_slash(child);
    if(strncmp(child, parent, parent_len) != 0 || child[parent_len] != '/') return 0;
    child += parent_len + 1;
    return child[0] && !fs_has_slash(child);
}

int fs_create(const char* name) {
    struct fs_entry* existing = fs_find(name);
    if(existing) return !fs_is_directory(existing);
    return fs_create_entry(name, 0);
}

int fs_mkdir(const char* name) {
    return fs_create_entry(name, 1);
}

int fs_delete(const char* name) {
    struct fs_entry* e = fs_find(name);
    if(!e || fs_is_directory(e)) return 0;
    struct fs_entry previous = *e;
    unsigned int previous_count = fs_sb.file_count;
    e->used = 0;
    e->name[0] = 0;
    e->size = 0;
    e->start_lba = 0;
    for(int i = 0; i < sizeof(e->reserved); i++) e->reserved[i] = 0;
    if(fs_sb.file_count) fs_sb.file_count--;
    if(!fs_save()) {
        *e = previous;
        fs_sb.file_count = previous_count;
        fs_save();
        return 0;
    }
    return 1;
}

int fs_rmdir(const char* name) {
    struct fs_entry* e = fs_find(name);
    if(!e || !fs_is_directory(e)) return 0;
    int len = strlen(name);
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_entry_valid(&fs_table[i]) &&
           strncmp(fs_table[i].name, name, len) == 0 &&
           fs_table[i].name[len] == '/')
            return 0;
    struct fs_entry previous = *e;
    unsigned int previous_count = fs_sb.file_count;
    e->used = 0;
    e->name[0] = 0;
    e->size = 0;
    e->start_lba = 0;
    for(int i = 0; i < sizeof(e->reserved); i++) e->reserved[i] = 0;
    if(fs_sb.file_count) fs_sb.file_count--;
    if(!fs_save()) {
        *e = previous;
        fs_sb.file_count = previous_count;
        fs_save();
        return 0;
    }
    return 1;
}

int fs_remove_tree(const char* name) {
    struct fs_entry* root = fs_find(name);
    if(!root || !fs_is_directory(root)) return 0;
    struct fs_entry previous[FS_MAX_FILES];
    for(int i = 0; i < FS_MAX_FILES; i++) previous[i] = fs_table[i];
    unsigned int previous_count = fs_sb.file_count;
    int len = strlen(name);
    int removed = 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        struct fs_entry* entry = &fs_table[i];
        if(!fs_entry_valid(entry)) continue;
        if(strcmp(entry->name, name) == 0 ||
           (strncmp(entry->name, name, len) == 0 && entry->name[len] == '/')) {
            entry->used = 0;
            entry->name[0] = 0;
            entry->size = 0;
            entry->start_lba = 0;
            for(int j = 0; j < sizeof(entry->reserved); j++) entry->reserved[j] = 0;
            removed++;
        }
    }
    if(!removed) return 0;
    if((unsigned int)removed > fs_sb.file_count) fs_sb.file_count = 0;
    else fs_sb.file_count -= removed;
    if(!fs_save()) {
        for(int i = 0; i < FS_MAX_FILES; i++) fs_table[i] = previous[i];
        fs_sb.file_count = previous_count;
        fs_save();
        return 0;
    }
    return 1;
}

int fs_rename(const char* old_name, const char* new_name) {
    struct fs_entry* source = fs_find(old_name);
    if(!source || !fs_name_valid(new_name) || !fs_parent_exists(new_name)) return 0;
    if(strcmp(old_name, new_name) == 0) return 1;
    if(fs_find(new_name)) return 0;

    int is_dir = fs_is_directory(source);
    int old_len = strlen(old_name);
    if(is_dir && strncmp(new_name, old_name, old_len) == 0 && new_name[old_len] == '/') return 0;

    char replacement[FS_MAX_FILES][FS_NAME_LEN];
    int changed[FS_MAX_FILES] = {0};
    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(!fs_entry_valid(&fs_table[i])) continue;
        if(strcmp(fs_table[i].name, old_name) != 0 &&
           !(is_dir && strncmp(fs_table[i].name, old_name, old_len) == 0 &&
             fs_table[i].name[old_len] == '/'))
            continue;
        int suffix = strlen(fs_table[i].name) - old_len;
        int new_len = strlen(new_name);
        if(new_len + suffix >= FS_NAME_LEN) return 0;
        int j = 0;
        while(new_name[j]) { replacement[i][j] = new_name[j]; j++; }
        for(int k = 0; k < suffix; k++) replacement[i][j++] = fs_table[i].name[old_len+k];
        replacement[i][j] = 0;
        changed[i] = 1;
    }
    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(!changed[i]) continue;
        for(int j = 0; j < FS_MAX_FILES; j++) {
            if(i == j || !fs_entry_valid(&fs_table[j])) continue;
            if(strcmp(replacement[i], fs_table[j].name) == 0 && !changed[j]) return 0;
        }
        for(int j = 0; j < i; j++)
            if(changed[j] && strcmp(replacement[i], replacement[j]) == 0) return 0;
    }
    struct fs_entry previous[FS_MAX_FILES];
    for(int i = 0; i < FS_MAX_FILES; i++) previous[i] = fs_table[i];
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(changed[i]) {
            int j = 0;
            while(replacement[i][j]) { fs_table[i].name[j] = replacement[i][j]; j++; }
            fs_table[i].name[j] = 0;
        }
    if(!fs_save()) {
        for(int i = 0; i < FS_MAX_FILES; i++) fs_table[i] = previous[i];
        fs_save();
        return 0;
    }
    return 1;
}

int fs_write(const char* name, const char* data, int len) {
    if(!data || len < 0) return 0;
    if(len > FS_MAX_FILE_BYTES) return 0;
    struct fs_entry* e = fs_find(name);
    int created = 0;
    if(!e) {
        if(!fs_create(name)) return 0;
        e = fs_find(name);
        created = 1;
    }
    if(!e || fs_is_directory(e)) return 0;
    for(int i = 0; i < FS_MAX_FILE_BYTES; i++) fs_iobuf[i] = 0;
    for(int i = 0; i < len; i++) fs_iobuf[i] = data[i];
    if(!ata_rw_sectors(e->start_lba, FS_MAX_FILE_SECTORS, (unsigned short*)fs_iobuf, 1)) {
        if(created) fs_delete(name);
        return 0;
    }
    unsigned int previous_size = e->size;
    e->size = len;
    if(!fs_save()) {
        e->size = previous_size;
        fs_save();
        if(created) fs_delete(name);
        return 0;
    }
    return 1;
}
int fs_read(const char* name, char* out, int max_len) {
    struct fs_entry* e = fs_find(name);
    if(!e || fs_is_directory(e) || !out || max_len <= 0) return -1;
    if(!ata_rw_sectors(e->start_lba, FS_MAX_FILE_SECTORS, (unsigned short*)fs_iobuf, 0)) return -1;
    int n = (int)e->size; if(n > max_len-1) n = max_len-1;
    for(int i = 0; i < n; i++) out[i] = fs_iobuf[i];
    out[n] = 0;
    return n;
}

int fs_check(void) {
    int errors = fs_sb.magic == FS_MAGIC ? 0 : 1;
    unsigned int valid_count = 0;
    unsigned int next_free = 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(!fs_table[i].used) continue;
        if(!fs_entry_valid(&fs_table[i])) {
            errors++;
            continue;
        }
        valid_count++;
        unsigned int entry_end = fs_table[i].start_lba + FS_MAX_FILE_SECTORS;
        if(entry_end > next_free) next_free = entry_end;
        if(!fs_parent_exists(fs_table[i].name)) errors++;
        for(int j = i + 1; j < FS_MAX_FILES; j++) {
            if(!fs_entry_valid(&fs_table[j])) continue;
            if(strcmp(fs_table[i].name, fs_table[j].name) == 0) errors++;
            if(fs_table[i].start_lba < fs_table[j].start_lba + FS_MAX_FILE_SECTORS &&
               fs_table[i].start_lba + FS_MAX_FILE_SECTORS > fs_table[j].start_lba)
                errors++;
        }
    }
    if(valid_count != fs_sb.file_count) errors++;
    if(next_free != fs_sb.next_free_lba) errors++;
    return errors;
}
