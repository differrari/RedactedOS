#include "p9_helper.h"
#include "syscalls/syscalls.h"
#include "memory/memory_access.h"
#include "memory/memory.h"

uint32_t mid;
uint32_t vfid;

void p9_max_tag(p9_packet_header* header){
    write_unaligned16(&header->tag,UINT16_MAX);
}

void p9_inc_tag(p9_packet_header* header){
    write_unaligned16(&header->tag,mid++);
}

void* make_p9_sized_buffer(size_t size){
    return page_alloc(size);
}

void* make_p9_response_buffer(){
    return make_p9_sized_buffer(PAGE_SIZE);
}

void p9_free(void *ptr){
    page_free(ptr);
}

void* make_p9_packet(size_t full_size, u16 id, bool max_tag){
    p9_packet_header *header = make_p9_sized_buffer(full_size);
    write_unaligned32(&header->size,full_size);
    header->id = id;
    if (max_tag)
        p9_max_tag(header);
    else
        p9_inc_tag(header);
    return header;
}

bool check_9p_success(void* buffer){
    p9_packet_header *header = buffer;
    return header->id != P9_RLERROR;
}

p9_version_packet* make_p9_version_packet(const char *version, u32 max_data_size){
    p9_version_packet *packet = make_p9_packet(sizeof(p9_packet_header) + sizeof(u32) + sizeof(u16) + strlen(version), P9_TVERSION, true);
    
    write_unaligned32(&packet->msize, max_data_size);
    write_unaligned16(&packet->str_size, strlen(version));
    memcpy(packet->buffer,version,strlen(version));
    return packet;
}

u32 read_p9_version_max_size(p9_version_packet* packet){
    return read_unaligned32(&packet->msize);
}

t_attach* make_p9_attach_packet(){
    t_attach *packet = make_p9_packet(sizeof(p9_packet_header) + sizeof(u32) + sizeof(u32) + sizeof(u16) + 8 + sizeof(u16) + sizeof(u32), P9_TATTACH, false);

    write_unaligned32(&packet->fid,vfid++);
    write_unaligned32(&packet->afid, UINT32_MAX);
    write_unaligned16(&packet->uname_len, 8); 
    memcpy(packet->payload,"REDACTED",8);
    
    u8 *cursor = packet->payload + 8;
    write_unaligned16((u16*)cursor, 0);
    cursor += sizeof(u16);
    write_unaligned32((u32*)cursor, 12345);//TODO: hash (name+timestamp) or random
    return packet;
}

t_lopen* make_p9_open_packet(u32 fid){
    t_lopen *packet = make_p9_packet(sizeof(t_lopen),P9_TLOPEN, false);

    write_unaligned32(&packet->fid,fid);
    write_unaligned32(&packet->flags,O_RDWR);
    
    return packet;
}

t_readdir* make_p9_readdir_packet(u32 fid, u32 size, u64 offset){
    t_readdir *packet = make_p9_packet(sizeof(t_readdir), P9_TREADDIR, false);
    
    write_unaligned32(&packet->fid,fid);
    write_unaligned32(&packet->count,size);
    write_unaligned64(&packet->offset,offset);
    
    return packet;
}

t_walk* make_p9_walk_packet(u32 fid, literal path){
    static char empty_path[] = "";
    const char *cursor = path;
    uint16_t names = 0;
    size_t full_size = sizeof(t_walk);

    while (*cursor == '/') cursor++;
    for (; *cursor; cursor++) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;

        const char *next = cursor;
        while (*next && *next != '/') next++;

        full_size += sizeof(uint16_t) + (size_t)(next - cursor);
        names++;
        cursor = next - 1;
    }

    t_walk *packet = make_p9_packet(full_size, P9_TWALK, false);
    
    write_unaligned32(&packet->fid,fid);
    write_unaligned32(&packet->newfid,vfid++);
    write_unaligned16(&packet->num_names,names);

    uintptr_t p = (uintptr_t)packet + sizeof(t_walk);

    cursor = path;
    while (*cursor == '/') cursor++;
    while (*cursor) {
        const char *next = cursor;
        while (*next && *next != '/') next++;
        size_t len = (size_t)(next - cursor);
        if (len){
            write_unaligned16((u16*)p, (u16)len);
            p += sizeof(uint16_t);
            memcpy((void*)p, cursor, len);
            p += len;
        }
        cursor = next;
        while (*cursor == '/') cursor++;
    }
    
    write_unaligned32(&packet->header.size,p-(uintptr_t)packet);
    return packet;
}

t_getattr* make_p9_getattr_packet(u32 fid, u64 mask){
    t_getattr *packet = make_p9_packet(sizeof(t_getattr), P9_TGETATTR, false);
    
    write_unaligned32(&packet->fid,fid);
    write_unaligned64(&packet->mask,mask);
    
    return packet;
}

t_setattr* make_p9_setattr_packet(u32 fid, u64 mask, u64 value){
    t_setattr *packet = make_p9_packet(sizeof(t_setattr), P9_TSETATTR, false);
    
    write_unaligned32(&packet->fid,fid);
    
    switch (mask) {
        case P9_SETATTR_MODE: write_unaligned32(&packet->mode, value & UINT32_MAX); break;
        case P9_SETATTR_UID: write_unaligned32(&packet->uid, value & UINT32_MAX); break;
        case P9_SETATTR_GID: write_unaligned32(&packet->gid, value & UINT32_MAX); break;
        case P9_SETATTR_SIZE: write_unaligned64(&packet->size, value); break;
        default: break;
    }
    
    write_unaligned32(&packet->valid, mask & UINT32_MAX);
    
    return packet;
}

t_read* make_p9_read_packet(u32 fid, u64 offset, u64 amount){
    t_read *packet = make_p9_packet(sizeof(t_read), P9_TREAD, false);
    write_unaligned32(&packet->fid,fid);
    write_unaligned64(&packet->offset,offset);
    write_unaligned32(&packet->count,amount);
    return packet;
}

t_write* make_p9_write_packet(u32 fid, u64 offset, size_t amount, const char* buf){
    t_write *packet = make_p9_packet(sizeof(t_write) + amount, P9_TWRITE, false);

    write_unaligned32(&packet->fid,fid);
    write_unaligned64(&packet->offset,offset);
    write_unaligned32(&packet->count, amount);
    memcpy((void*)((uptr)packet + sizeof(t_write)), buf, amount);
    return packet;
}

t_clunk* make_p9_clunk_packet(u32 fid){
    t_clunk *packet = make_p9_packet(sizeof(t_clunk), P9_TCLUNK, false);
    write_unaligned32(&packet->fid, fid);
    return packet;
}
