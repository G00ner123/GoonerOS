#include "kernel.h"
#include "io.h"

struct fs_entry fs_table[FS_MAX_FILES];
struct fs_superblock fs_sb;
static unsigned char fs_iobuf[FS_MAX_FILE_BYTES];

void fs_save(void) {
    ata_rw_sectors(FS_SUPERBLOCK_LBA, 1, (unsigned short*)&fs_sb, 1);
    ata_rw_sectors(FS_TABLE_LBA, FS_TABLE_SECTORS, (unsigned short*)fs_table, 1);
}
void fs_load(void) {
    int ok = ata_rw_sectors(FS_SUPERBLOCK_LBA, 1, (unsigned short*)&fs_sb, 0);
    ok = ok && ata_rw_sectors(FS_TABLE_LBA, FS_TABLE_SECTORS, (unsigned short*)fs_table, 0);
    if(!ok) {
        // Platte antwortet nicht (z.B. kein/falsch konfiguriertes Laufwerk in QEMU) ->
        // ohne persistentes Dateisystem weiterstarten statt den Kernel haengen zu lassen
        fs_sb.magic = FS_MAGIC;
        fs_sb.file_count = 0;
        fs_sb.next_free_lba = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) fs_table[i].used = 0;
        return;
    }
    if(fs_sb.magic != FS_MAGIC) {
        // Noch kein gueltiges Dateisystem auf der Platte -> frisch anlegen
        fs_sb.magic = FS_MAGIC;
        fs_sb.file_count = 0;
        fs_sb.next_free_lba = 0;
        for(int i = 0; i < FS_MAX_FILES; i++) fs_table[i].used = 0;
        fs_save();
    }
}

struct fs_entry* fs_find(const char* name) {
    for(int i = 0; i < FS_MAX_FILES; i++)
        if(fs_table[i].used && strcmp(fs_table[i].name, name) == 0) return &fs_table[i];
    return 0;
}
int fs_create(const char* name) {
    if(fs_find(name)) return 1;
    if(strlen(name) >= FS_NAME_LEN) return 0;
    for(int i = 0; i < FS_MAX_FILES; i++) {
        if(!fs_table[i].used) {
            int j = 0; while(name[j] && j < FS_NAME_LEN-1) { fs_table[i].name[j] = name[j]; j++; }
            fs_table[i].name[j] = 0;
            fs_table[i].size = 0;
            fs_table[i].start_lba = FS_DATA_START_LBA + fs_sb.next_free_lba;
            fs_table[i].used = 1;
            fs_sb.next_free_lba += FS_MAX_FILE_SECTORS;
            fs_sb.file_count++;
            fs_save();
            return 1;
        }
    }
    return 0; // Dateitabelle voll
}
int fs_delete(const char* name) {
    struct fs_entry* e = fs_find(name);
    if(!e) return 0;
    e->used = 0;
    fs_sb.file_count--;
    fs_save();
    return 1;
}
int fs_write(const char* name, const char* data, int len) {
    struct fs_entry* e = fs_find(name);
    if(!e) { if(!fs_create(name)) return 0; e = fs_find(name); }
    if(len > FS_MAX_FILE_BYTES) len = FS_MAX_FILE_BYTES;
    for(int i = 0; i < FS_MAX_FILE_BYTES; i++) fs_iobuf[i] = 0;
    for(int i = 0; i < len; i++) fs_iobuf[i] = data[i];
    ata_rw_sectors(e->start_lba, FS_MAX_FILE_SECTORS, (unsigned short*)fs_iobuf, 1);
    e->size = len;
    fs_save();
    return 1;
}
int fs_read(const char* name, char* out, int max_len) {
    struct fs_entry* e = fs_find(name);
    if(!e) return -1;
    ata_rw_sectors(e->start_lba, FS_MAX_FILE_SECTORS, (unsigned short*)fs_iobuf, 0);
    int n = (int)e->size; if(n > max_len-1) n = max_len-1;
    for(int i = 0; i < n; i++) out[i] = fs_iobuf[i];
    out[n] = 0;
    return n;
}
