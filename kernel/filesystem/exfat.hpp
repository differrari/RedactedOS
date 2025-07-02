#pragma once

#include "types.h"
#include "std/string.h"

typedef struct exfat_mbs {
    uint8_t jumpboot[3];//3
    char fsname[8];//8
    uint8_t mustbezero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_directory;
    uint32_t volume_serial_number;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t bytes_per_sector_shift;
    uint8_t sectors_per_cluster_shift;
    uint8_t number_of_fats;
    uint8_t drive_select;
    uint8_t percent_in_use;
    uint8_t rsvd[7];
    char bootcode[390];
    uint16_t bootsignature;
}__attribute__((packed)) exfat_mbs;

typedef struct file_entry {
    uint8_t entry_type;
    uint8_t entry_count;
    uint16_t checksum;
    struct {
        uint16_t read_only: 1;
        uint16_t hidden: 1;
        uint16_t system: 1;
        uint16_t rsvd: 1;
        uint16_t directory: 1;
        uint16_t archive: 1;
        uint16_t rsvd2: 10;
    } flags;
    
    uint16_t rsvd;
    uint32_t create_timestamp;
    uint32_t last_modified;
    uint32_t last_accessed;
    uint8_t create10msincrement;
    uint8_t lastmod10msincrement;
    uint8_t createutcoffset;
    uint8_t lastmodutcoffset;
    uint8_t lastaccutcoffset;
    
    uint8_t rsvd2[7];
}__attribute__((packed)) file_entry;

typedef struct fileinfo_entry {
        uint8_t entry_type;
        uint8_t flags;
        uint8_t rsvd;
        uint8_t name_length;
        uint16_t name_hash;
        uint16_t rsvd2;
        uint64_t valid_filesize;
        uint32_t rsvd3;
        uint32_t first_cluster;
        uint64_t filesize;
}__attribute__((packed)) fileinfo_entry;

typedef struct filename_entry {
        uint8_t entry_type;
        uint8_t flags;
        uint16_t name[15];
}__attribute__((packed)) filename_entry;

class ExFATFS;

typedef void* (*ef_entry_handler)(ExFATFS *instance, file_entry*, fileinfo_entry*, filename_entry*, char *seek);

class ExFATFS {
public:
    bool init();
    void* read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index);
    void* read_file(char *path);
    string_list* list_contents(char *path);

protected:
    void read_FAT(uint32_t location, uint32_t size, uint8_t count);
    void* list_directory(uint32_t cluster_count, uint32_t root_index);
    void* walk_directory(uint32_t cluster_count, uint32_t root_index, char *seek, ef_entry_handler handler);
    void* read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index);
    char* advance_path(char *path);

    exfat_mbs* mbs;
    void *fs_page;
    
    static void* read_entry_handler(ExFATFS *instance, file_entry *entry, fileinfo_entry *info, filename_entry *name, char *seek);
    static void* list_entries_handler(ExFATFS *instance, file_entry *entry, fileinfo_entry *info, filename_entry *name, char *seek);
};