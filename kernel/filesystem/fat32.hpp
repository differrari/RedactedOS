#pragma once

#include "types.h"
#include "std/string.h"

typedef struct fat32_mbs {
    uint8_t jumpboot[3];//3
    char fsname[8];//8
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t number_of_fats;
    uint16_t root_directory_entries;
    uint16_t num_sectors;//Could be 0, in which case it's stored in large_num_sectors
    uint8_t media_descriptor_type;
    uint16_t num_sectors_fat;//Used only in 12/16
    uint16_t num_sectors_track;
    uint16_t num_heads;
    uint32_t num_hidden_sectors;
    uint32_t large_num_sectors;
    
    uint32_t sectors_per_fat;
    uint16_t flags;
    uint16_t fat_version;//high major low minor
    uint32_t first_cluster_of_root_directory;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved_flags;
    uint8_t signature;//0x28 or 0x29
    uint32_t volume_id;
    char volume_label[11];
    char system_identifier[8];
    uint8_t boot_code[420];
    uint16_t boot_signature;
}__attribute__((packed)) fat32_mbs;

typedef struct f32file_entry {
    char filename[11];
    struct {
        uint8_t read_only: 1;
        uint8_t hidden: 1;
        uint8_t system: 1;
        uint8_t volume_id: 1;
        uint8_t directory: 1;
        uint8_t archive: 1;
        uint8_t long_filename: 1;
    } flags;
    
    uint8_t rsvd;//Can be used for case sensitivity

    uint8_t create10msincrement;
    uint16_t create_timestamp;//Multiply seconds by 2
    uint16_t create_date;
    uint16_t last_accessed_date;
    uint16_t hi_first_cluster;
    uint16_t last_modified_time;
    uint16_t last_modified_date;
    uint16_t lo_first_cluster;
    uint32_t filesize;

}__attribute__((packed)) f32file_entry;

typedef struct f32longname {
        uint8_t order;
        uint16_t name1[5];
        uint8_t attribute;
        uint8_t long_type;
        uint8_t checksum;
        uint16_t name2[6];
        uint16_t rsvd2;
        uint16_t name3[2];
}__attribute__((packed)) f32longname;

class FAT32FS;

typedef void* (*f32_entry_handler)(FAT32FS *instance, f32file_entry*, char *filename, char *seek);

class FAT32FS {
public:
    bool init();
    void* read_file(char *path);
    string_list* list_contents(char *path);
    
protected:
    void* read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index);
    void read_FAT(uint32_t location, uint32_t size, uint8_t count);
    uint32_t count_FAT(uint32_t first);
    void* list_directory(uint32_t cluster_count, uint32_t root_index);
    void* walk_directory(uint32_t cluster_count, uint32_t root_index, char *seek, f32_entry_handler handler);
    void* read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index);
    char* advance_path(char *path);

    fat32_mbs* mbs;
    void *fs_page;
    uint32_t cluster_count;
    uint32_t data_start_sector;
    uint32_t* fat;
    uint32_t total_fat_entries;

    static void* read_entry_handler(FAT32FS *instance, f32file_entry *entry, char *filename, char *seek);
    static void* list_entries_handler(FAT32FS *instance, f32file_entry *entry, char *filename, char *seek);

    void parse_longnames(f32longname entries[], uint16_t count, char* out);
    void parse_shortnames(f32file_entry* entry, char* out);
};