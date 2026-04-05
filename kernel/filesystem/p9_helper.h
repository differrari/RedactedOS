#pragma once

#include "types.h"

typedef struct p9_packet_header {
    uint32_t size;
    uint8_t id;
    uint16_t tag;
}__attribute__((packed)) p9_packet_header;

enum {
    P9_TLERROR = 6,
    P9_RLERROR,
    P9_TSTATFS = 8,
    P9_RSTATFS,
    P9_TLOPEN = 12,
    P9_RLOPEN,
    P9_TLCREATE = 14,
    P9_RLCREATE,
    P9_TSYMLINK = 16,
    P9_RSYMLINK,
    P9_TMKNOD = 18,
    P9_RMKNOD,
    P9_TRENAME = 20,
    P9_RRENAME,
    P9_TREADLINK = 22,
    P9_RREADLINK,
    P9_TGETATTR = 24,
    P9_RGETATTR,
    P9_TSETATTR = 26,
    P9_RSETATTR,
    P9_TXATTRWALK = 30,
    P9_RXATTRWALK,
    P9_TXATTRCREATE = 32,
    P9_RXATTRCREATE,
    P9_TREADDIR = 40,
    P9_RREADDIR,
    P9_TFSYNC = 50,
    P9_RFSYNC,
    P9_TLOCK = 52,
    P9_RLOCK,
    P9_TGETLOCK = 54,
    P9_RGETLOCK,
    P9_TLINK = 70,
    P9_RLINK,
    P9_TMKDIR = 72,
    P9_RMKDIR,
    P9_TRENAMEAT = 74,
    P9_RRENAMEAT,
    P9_TUNLINKAT = 76,
    P9_RUNLINKAT,
    P9_TVERSION = 100,
    P9_RVERSION,
    P9_TAUTH = 102,
    P9_RAUTH,
    P9_TATTACH = 104,
    P9_RATTACH,
    P9_TERROR = 106,
    P9_RERROR,
    P9_TFLUSH = 108,
    P9_RFLUSH,
    P9_TWALK = 110,
    P9_RWALK,
    P9_TOPEN = 112,
    P9_ROPEN,
    P9_TCREATE = 114,
    P9_RCREATE,
    P9_TREAD = 116,
    P9_RREAD,
    P9_TWRITE = 118,
    P9_RWRITE,
    P9_TCLUNK = 120,
    P9_RCLUNK,
    P9_TREMOVE = 122,
    P9_RREMOVE,
    P9_TSTAT = 124,
    P9_RSTAT,
    P9_TWSTAT = 126,
    P9_RWSTAT,
};

#ifdef __cplusplus
extern "C" {
#endif

void* make_p9_sized_buffer(size_t size);
void* make_p9_response_buffer();
void p9_free(void *ptr);

void p9_max_tag(p9_packet_header* header);
void p9_inc_tag(p9_packet_header* header);

bool check_9p_success(void* buffer);

typedef struct p9_version_packet {
    p9_packet_header header;
    u32 msize;
    u16 str_size;
    char buffer[8];
}__attribute__((packed)) p9_version_packet;
p9_version_packet* make_p9_version_packet(const char *version, u32 max_data_size);
u32 read_p9_version_max_size(p9_version_packet* packet);

typedef struct t_attach {
    p9_packet_header header;
    uint32_t fid;
    uint32_t afid;
    uint16_t uname_len;
    char uname[8];
    uint16_t aname_len;
    char aname[1];
    uint32_t n_uname;
}__attribute__((packed)) t_attach;

t_attach* make_p9_attach_packet();

typedef struct r_attach {
    p9_packet_header header;
    uint8_t qid[13];
}__attribute__((packed)) r_attach;

#define O_RDONLY         00
#define O_WRONLY         01
#define O_RDWR           02

typedef struct t_lopen {
    p9_packet_header header;
    uint32_t fid; 
    uint32_t flags;
}__attribute__((packed)) t_lopen;

typedef struct r_lopen {
    p9_packet_header header;
    uint8_t qid[13]; 
    uint32_t iounit;
}__attribute__((packed)) r_lopen;

t_lopen* make_p9_open_packet(u32 fid);

typedef struct t_readdir {
    p9_packet_header header;
    uint32_t fid;
    uint64_t offset;
    uint32_t count;
}__attribute__((packed)) t_readdir;

typedef struct r_readdir_data {
    uint8_t qid[13];
    uint64_t offset;
    uint8_t type;
    uint16_t name_len;
    // Followed by name;
}__attribute__((packed)) r_readdir_data;

t_readdir* make_p9_readdir_packet(uint32_t fid, u32 size, uint64_t offset);

typedef struct r_readdir {
    p9_packet_header header;
    uint32_t count;
    // Followed by data
}__attribute__((packed)) r_readdir;

typedef struct t_walk {
    p9_packet_header header;
    uint32_t fid;
    uint32_t newfid;
    uint16_t num_names;
}__attribute__((packed)) t_walk;

t_walk* make_p9_walk_packet(u32 fid, literal path);

#define P9_GETATTR_MODE         0x00000001ULL
#define P9_GETATTR_NLINK        0x00000002ULL
#define P9_GETATTR_UID          0x00000004ULL
#define P9_GETATTR_GID          0x00000008ULL
#define P9_GETATTR_RDEV         0x00000010ULL
#define P9_GETATTR_ATIME        0x00000020ULL
#define P9_GETATTR_MTIME        0x00000040ULL
#define P9_GETATTR_CTIME        0x00000080ULL
#define P9_GETATTR_INO          0x00000100ULL
#define P9_GETATTR_SIZE         0x00000200ULL
#define P9_GETATTR_BLOCKS       0x00000400ULL

#define P9_GETATTR_BTIME        0x00000800ULL
#define P9_GETATTR_GEN          0x00001000ULL
#define P9_GETATTR_DATA_VERSION 0x00002000ULL

#define P9_GETATTR_BASIC        0x000007ffULL /* Mask for fields up to BLOCKS */
#define P9_GETATTR_ALL          0x00003fffULL /* Mask for All fields above */

typedef struct t_getattr {
    p9_packet_header header;
    uint32_t fid;
    uint64_t mask;    
}__attribute__((packed)) t_getattr;

typedef struct r_getattr {
    p9_packet_header header;
    uint64_t valid;
    uint8_t qid[13];
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t nlink;
    uint64_t rdev;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, btime_sec, btime_nsec;
    uint64_t gen;
    uint64_t data_version;
}__attribute__((packed)) r_getattr;

t_getattr* make_p9_getattr_packet(u32 fid, u64 mask);

#define P9_SETATTR_MODE         0x00000001UL
#define P9_SETATTR_UID          0x00000002UL
#define P9_SETATTR_GID          0x00000004UL
#define P9_SETATTR_SIZE         0x00000008UL
#define P9_SETATTR_ATIME        0x00000010UL
#define P9_SETATTR_MTIME        0x00000020UL
#define P9_SETATTR_CTIME        0x00000040UL
#define P9_SETATTR_ATIME_SET    0x00000080UL
#define P9_SETATTR_MTIME_SET    0x00000100UL

typedef struct {
     p9_packet_header header;
     u32 fid;
     u32 valid;
     u32 mode;
     u32 uid;
     u32 gid;
     u64 size;
     u64 atime_sec;
     u64 atime_nsec;
     u64 mtime_sec;
     u64 mtime_nsec;
}__attribute__((packed)) t_setattr;

t_setattr* make_p9_setattr_packet(u32 fid, u64 mask, u64 value);

typedef struct t_read {
    p9_packet_header header;
    uint32_t fid;
    uint64_t offset;
    uint32_t count;
}__attribute__((packed)) t_read;

t_read* make_p9_read_packet(u32 fid, u64 offset, u64 amount);

typedef struct t_write {
    p9_packet_header header;
    u32 fid;
    u64 offset;
    u32 count;
    //Followed by data
}__attribute__((packed)) t_write;

typedef struct r_read {
    p9_packet_header header;
    u32 count;
}__attribute__((packed)) r_write;

t_write* make_p9_write_packet(u32 fid, u64 offset, size_t amount, const char* buf);

#ifdef __cplusplus
}
#endif