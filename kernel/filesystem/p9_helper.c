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
    // page_free(ptr);
}

void* make_p9_packet(size_t full_size, u16 id, bool max_tag){
    p9_packet_header *header = page_alloc(PAGE_SIZE);
    header->size = sizeof(p9_version_packet);
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
    p9_version_packet *packet = make_p9_packet(sizeof(p9_version_packet), P9_TVERSION, true);
    
    write_unaligned32(&packet->msize, max_data_size);
    write_unaligned16(&packet->str_size, strlen(version));
    memcpy(packet->buffer,version,8);
    return packet;
}

u32 read_p9_version_max_size(p9_version_packet* packet){
    return read_unaligned32(&packet->msize);
}

t_attach* make_p9_attach_packet(){
    t_attach *packet = make_p9_packet(sizeof(t_attach),P9_TATTACH, false);

    write_unaligned32(&packet->fid,vfid++);
    write_unaligned16(&packet->uname_len, 8); 
    write_unaligned32(&packet->n_uname,12345);//TODO: hash (name+timestamp) or random
    memcpy(packet->uname,"REDACTED",8);
    
    return packet;
}

t_lopen* make_p9_open_packet(u32 fid){
    t_lopen *packet = make_p9_packet(sizeof(t_lopen),P9_TLOPEN, false);

    write_unaligned32(&packet->fid,fid);
    write_unaligned32(&packet->flags,O_RDONLY);
    
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
    t_walk *packet = make_p9_packet(sizeof(t_walk), P9_TWALK, false);
    uint32_t nfid = vfid++;
    
    write_unaligned32(&packet->fid,fid);
    write_unaligned32(&packet->newfid,nfid);

    uintptr_t p = (uintptr_t)packet + sizeof(t_walk);

    char *new_path = path;
    while (*new_path != 0) {
        new_path = (char*)seek_to(new_path, '/');
        uint8_t offset = new_path-path-(*new_path != 0 || *(new_path-1) == '/');
        if (offset != 0){
            write_unaligned16(&packet->num_names,read_unaligned16(&packet->num_names) + 1);
            *(uint16_t*)p = offset;
            p += 2;
            memcpy((void*)p, path, offset);
            p += offset;
        }
        path = new_path;
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

t_read* make_p9_read_packet(u32 fid, u64 offset, u64 amount){
    t_read *packet = make_p9_packet(sizeof(t_read), P9_TREAD, false);
    write_unaligned32(&packet->fid,fid);
    write_unaligned64(&packet->offset,offset);
    write_unaligned32(&packet->count,amount - sizeof(p9_packet_header) - sizeof(u32));
    return packet;
}