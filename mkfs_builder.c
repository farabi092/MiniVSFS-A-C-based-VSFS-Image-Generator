// gcc-o mkfs_builder mkfs_builder.c
// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <ctype.h>


#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint32_t CRC32_TAB[256];
static void crc32_init(void){
    uint32_t i=0;
    while (i<256) {
        uint32_t c=i;
        int j=0;
        while (j<8) {
            if (c&1)
                c=0xEDB88320u ^ (c>>1);
            else
                c=c>>1;
            j++;
        }
        CRC32_TAB[i]=c;
        i++;
    }
}
static uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}


#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;

#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must be 116 bytes");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t  type;
    char     name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


static uint32_t superblock_crc_finalize(superblock_t *sb_block_aligned) {
    sb_block_aligned->checksum = 0;
    return sb_block_aligned->checksum = crc32((void *) sb_block_aligned, BS - 4);
}
static void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}
static void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0; for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}


static inline uint64_t ceil_div_u64(uint64_t a, uint64_t b){ return (a + b - 1u) / b; }
static inline void set_bit(uint8_t *bm, uint64_t i){ bm[i>>3] |= (uint8_t)(1u << (i & 7u)); }


typedef struct {
    const char *image;
    uint64_t size_kib;   // in KiB
    uint64_t inodes;
} args_t;


static int parse_size_kib(const char *s, uint64_t *out_kib) {
    if (!s) return -1;
    size_t len = strlen(s);
    if (len == 0) return -1;
    char last = s[len-1];
    if (last=='M' || last=='m') {

        long val = strtol(s, NULL, 10);
        if (val <= 0) return -1;
        *out_kib = (uint64_t)val * 1024u;
        return 0;
    } else if (last=='K' || last=='k') {
        long val = strtol(s, NULL, 10);
        if (val <= 0) return -1;
        *out_kib = (uint64_t)val;
        return 0;
    } else {

        long val = strtol(s, NULL, 10);
        if (val <= 0) return -1;
        *out_kib = (uint64_t)val;
        return 0;
    }
}


static int parse_args(int argc, char **argv, args_t *out) {
    out->image = NULL; out->size_kib = 0; out->inodes = 128; 
    if (argc==3) {
       
        out->image = argv[1];
        if (parse_size_kib(argv[2], &out->size_kib) != 0) {
            fprintf(stderr, "Bad size: %s\n", argv[2]); return -1;
        }
        return 0;
    }
    
    int i=1;
    while (i<argc) {
    //for (int i=1;i<argc;i++){
        if (!strcmp(argv[i], "--image") && i + 1 < argc) {
           out->image = argv[++i];
        } 
        else if (!strcmp(argv[i], "--size-kib") && i + 1 < argc) {
           out->size_kib = strtoull(argv[++i], NULL, 10);
        } 
        else if (!strcmp(argv[i], "--inodes") && i + 1 < argc) {
           out->inodes = strtoull(argv[++i], NULL, 10);
        } 
        else {
           fprintf(stderr, "Unknown/Bad arg: %s\n", argv[i]);
           return -1;
        }
        i++;
    }
    if (!out->image || out->size_kib == 0) {
       fprintf(stderr, "Usage:\n  %s disk.img 1M\n  or: %s --image out.img --size-kib 1024 --inodes 128\n", argv[0], argv[0]);
       return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    crc32_init();

    args_t a;
    if (parse_args(argc, argv, &a) != 0) return 1;


    if (a.size_kib < 180 || a.size_kib > 4096 || (a.size_kib % 4) != 0) {
        fprintf(stderr, "size-kib must be 180..4096 and multiple of 4 (you gave %" PRIu64 ")\n", a.size_kib);
        return 1;
    }
    if (a.inodes < 128 || a.inodes > 512) {
        fprintf(stderr, "inodes must be 128..512 (you gave %" PRIu64 ")\n", a.inodes);
        return 1;
    }

    const uint64_t total_bytes  = a.size_kib * 1024u;
    const uint64_t total_blocks = total_bytes / BS;

    const uint64_t inode_table_blocks = ceil_div_u64(a.inodes * INODE_SIZE, BS);
    const uint64_t superblock_start   = 0;
    const uint64_t inode_bm_start     = 1;
    const uint64_t data_bm_start      = 2;
    const uint64_t inode_table_start  = 3;
    const uint64_t data_region_start  = inode_table_start + inode_table_blocks;

    if (data_region_start >= total_blocks) {
        fprintf(stderr, "Image too small for requested inode count.\n");
        return 1;
    }
    const uint64_t data_region_blocks = total_blocks - data_region_start;

    uint8_t *img = (uint8_t*)calloc(1, total_bytes);
    if (!img) { perror("calloc"); return 1; }
    #define BLK(i) (img + (uint64_t)(i) * BS)


    superblock_t *sb = (superblock_t*)BLK(superblock_start);
    memset(sb, 0, sizeof(*sb));
    sb->magic = 0x4D565346u;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count  = a.inodes;
    sb->inode_bitmap_start   = inode_bm_start;
    sb->inode_bitmap_blocks  = 1;
    sb->data_bitmap_start    = data_bm_start;
    sb->data_bitmap_blocks   = 1;
    sb->inode_table_start    = inode_table_start;
    sb->inode_table_blocks   = inode_table_blocks;
    sb->data_region_start    = data_region_start;
    sb->data_region_blocks   = data_region_blocks;
    sb->root_inode  = ROOT_INO;
    sb->mtime_epoch = (uint64_t)time(NULL);
    sb->flags = 0;
    superblock_crc_finalize((superblock_t*)BLK(0));


    uint8_t *ibm = BLK(inode_bm_start);
    set_bit(ibm, 0); 


    uint8_t *dbm = BLK(data_bm_start);
    set_bit(dbm, 0); 

 
    inode_t *root = (inode_t*)(BLK(inode_table_start) + 0);
    memset(root, 0, sizeof(*root));
    root->mode  = 0040000;
    root->links = 2;
    root->uid = 0; root->gid = 0;
    root->size_bytes = BS;
    uint64_t now = (uint64_t)time(NULL);
    root->atime = now; root->mtime = now; root->ctime = now;
    uint32_t root_data_abs = (uint32_t)(data_region_start + 0);
    root->direct[0] = root_data_abs;
    inode_crc_finalize(root);

  
    uint8_t *dirblk = BLK(root_data_abs);
    dirent64_t *de0 = (dirent64_t*)dirblk;
    memset(de0, 0, sizeof(*de0));
    de0->inode_no = ROOT_INO; de0->type = 2; strcpy(de0->name, ".");
    dirent_checksum_finalize(de0);
    dirent64_t *de1 = (dirent64_t*)(dirblk + sizeof(dirent64_t));
    memset(de1, 0, sizeof(*de1));
    de1->inode_no = ROOT_INO; de1->type = 2; strcpy(de1->name, "..");
    dirent_checksum_finalize(de1);


    FILE *f = fopen(a.image, "wb");
    if (!f){ perror("fopen"); free(img); return 1; }
    size_t wrote = fwrite(img, 1, total_bytes, f);
    if (wrote != total_bytes) { fprintf(stderr, "Short write\n"); fclose(f); free(img); return 1; }
    fclose(f);
    free(img);

    printf("Created MiniVSFS image '%s' (%" PRIu64 " KiB, %" PRIu64 " blocks) with %" PRIu64 " inodes\n",
           a.image, a.size_kib, total_blocks, a.inodes);
    return 0;
}

