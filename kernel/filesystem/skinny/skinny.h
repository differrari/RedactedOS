#pragma once

#include "types.h"

typedef struct {
    char signature[8];
    uintptr_t root_chunk;
    uint64_t version;
    uint8_t sectors_per_chunk;
    uint8_t rsvd[487];
    uintptr_t next_free_chunk;
} skinnyfs_header;

typedef struct {
    uintptr_t entry_ptr;
    size_t size;
    char name[95];
    uint8_t attributes;
    uintptr_t list_ptr;
    uintptr_t rsvd;//Could extend fs_entry with extra data
} skinnyfs_entry;

typedef struct {
    uintptr_t chunk_list[63];//Based off chunk size = 1 sector. It's (chunkbytesize-8)/8
    uintptr_t next_list;
} skinnyfs_chunk_list;

//Read root chunk
//A directory is a list of entries, fixed due to chunk size and each entry being 128bytes
//Read entries, if reaching the last one, it's a pointer to another chunk where the directory continues

//Read an entry's size, it'll say how much to read.
//Read one chunk off the entry_ptr
//If that's not enough, follow the list pointer
    //Read off the list pointer's pointers
    //If that's still not enough, go to next_list and read that

//When writing a new file:
    //Follow down to the directory
    //Find its end, find the last entry (attribute last)
    //Make it not the last one, add a new one
    //If not enough space in current chunk, allocate a new one *

//When appending to an existing file
    //Find its tail (follow entry-ptr and list-ptr until size is done)
    //Write at end of chunk
    //Alloc a new chunk if needed

//Allocate new chunks:
    //Follow the next free chunk
    //The first 8 bytes of a chunk point to the next free chunk
    //Follow until 0
    //The pointer to that one will be replaced with a 0 and become the new tail