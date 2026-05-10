// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
// gcc -o mkfs_adder mkfs_adder.c
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#pragma pack(push,1)

typedef struct {
    uint32_t magic, version, block_size;
    uint64_t total_blocks, inode_count;
    uint64_t inode_bitmap_start, inode_bitmap_blocks;
    uint64_t data_bitmap_start,  data_bitmap_blocks;
    uint64_t inode_table_start,  inode_table_blocks;
    uint64_t data_region_start,  data_region_blocks;
    uint64_t root_inode, mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t)==116, "superblock mismatch");

#pragma pack(push,1)
typedef struct {
    uint16_t mode; uint16_t links;
    uint32_t uid,gid;
    uint64_t size_bytes;
    uint64_t atime,mtime,ctime;
    uint32_t direct[12];
    uint32_t reserved_0,reserved_1,reserved_2;
    uint32_t proj_id,uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE,"inode mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64,"dirent mismatch");


uint32_t CRC32_TAB[256];
static void crc32_init(void){
    for(uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
static uint32_t crc32(const void*data,size_t n){
    const uint8_t*p=data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c=CRC32_TAB[(c^p[i])&0xFF]^(c>>8);
    return c^0xFFFFFFFFu;
}
static void inode_crc_finalize(inode_t*ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp,ino,INODE_SIZE);
    memset(&tmp[120],0,8);
    uint32_t c=crc32(tmp,120);
    ino->inode_crc=(uint64_t)c;
}
static void dirent_checksum_finalize(dirent64_t* de){
    const uint8_t* p=(const uint8_t*)de; uint8_t x=0;
    for(int i=0;i<63;i++) x^=p[i];
    de->checksum=x;
}


static inline void set_bit(uint8_t*bm,uint64_t i){ bm[i>>3]|=(1u<<(i&7u)); }
static inline int  test_bit(uint8_t*bm,uint64_t i){ return (bm[i>>3]>>(i&7u))&1; }
static inline int  find_free_bit(uint8_t*bm,uint64_t maxbits){
    for(uint64_t i=0;i<maxbits;i++) if(!test_bit(bm,i)) return (int)i;
    return -1;
}




int main(int argc,char**argv){
    if(argc!=4){
        fprintf(stderr,"Usage: %s <image> <hostfile> <fsname>\n",argv[0]);
        return 1;
    }
    const char* imgfile=argv[1];
    const char* hostfile=argv[2];
    const char* fsname=argv[3];

    crc32_init();


    FILE*f=fopen(imgfile,"r+b");
    if(!f){ perror("Error: cannot open the input file as it is not in the host system!!!"); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f);
    rewind(f);
    uint8_t* img=malloc(sz);
    if(!img){ perror("malloc"); return 1; }
    fread(img,1,sz,f);

    #define BLK(i) (img+(uint64_t)(i)*BS)

    superblock_t* sb=(superblock_t*)BLK(0);


    inode_t* root=(inode_t*)(BLK(sb->inode_table_start)+0);
    uint32_t rootblk=root->direct[0];
    uint8_t* dirblk=BLK(rootblk);

    int entries_per_block = BS / sizeof(dirent64_t);


    for(int i=0;i<entries_per_block;i++){
        dirent64_t* de=(dirent64_t*)(dirblk + i*sizeof(dirent64_t));
        if(de->inode_no!=0 && strcmp(de->name,fsname)==0){
            fprintf(stderr,"Error: file '%s' already exists in VSFS\n", fsname);
            free(img);
            fclose(f);
            return 1;
        }
    }

    uint8_t* ibm=BLK(sb->inode_bitmap_start);
    uint8_t* dbm=BLK(sb->data_bitmap_start);

    int ino_idx=find_free_bit(ibm,sb->inode_count);
    if(ino_idx<0){ fprintf(stderr,"No free inode\n"); free(img); fclose(f); return 1; }
    set_bit(ibm,ino_idx);
    uint32_t inode_no=ino_idx+1; 


    int db_idx=find_free_bit(dbm,sb->data_region_blocks);
    if(db_idx<0){ fprintf(stderr,"No free data block\n"); free(img); fclose(f); return 1; }
    set_bit(dbm,db_idx);
    uint32_t data_abs=(uint32_t)(sb->data_region_start+db_idx);


    FILE*hf=fopen(hostfile,"rb");
    if(!hf){ perror("Cannot open input file as it is not in host system!"); free(img); fclose(f); return 1; }
    fseek(hf,0,SEEK_END); long fsz=ftell(hf); rewind(hf);
    if(fsz>BS){ fprintf(stderr,"File too big (only 1 block supported)\n"); free(img); fclose(f); return 1; }
    fread(BLK(data_abs),1,fsz,hf);
    fclose(hf);


    inode_t* ino=(inode_t*)(BLK(sb->inode_table_start)+ino_idx*INODE_SIZE);
    memset(ino,0,sizeof(*ino));
    ino->mode=0100000; 
    ino->links=1;
    ino->uid=0; ino->gid=0;
    ino->size_bytes=fsz;
    uint64_t now=time(NULL);
    ino->atime=now; ino->mtime=now; ino->ctime=now;
    ino->direct[0]=data_abs;
    inode_crc_finalize(ino);

    int added=0;
    for(int i=0;i<entries_per_block;i++){
        dirent64_t* de=(dirent64_t*)(dirblk + i*sizeof(dirent64_t));
        if(de->inode_no==0){
            memset(de,0,sizeof(dirent64_t));
            de->inode_no=inode_no;
            de->type=1;
            strncpy(de->name,fsname,sizeof(de->name)-1);
            dirent_checksum_finalize(de);
            added=1;
            break;
        }
    }
    if(!added){ fprintf(stderr,"Root directory full\n"); free(img); fclose(f); return 1; }

    rewind(f);
    fwrite(img,1,sz,f);
    fclose(f);
    free(img);

    printf("Added file '%s' (inode=%u, %ld bytes)\n",fsname,inode_no,fsz);
    return 0;
}
