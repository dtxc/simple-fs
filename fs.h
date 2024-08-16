#pragma once

#include <stdio.h>
#include <stdint.h>


#define FS_DIR      (1 << 0)
#define FS_FILE     (1 << 1)
#define FS_CHARDEV  (1 << 2)

#define NODE_MAGIC  0x7F9C41
#define FS_MAGIC    "SFS PART"

#define BLOCK_SIZE 4096
#define BLOCK_DATA_SIZE (BLOCK_SIZE - 4 - 1 - 3)

#define BLOCK_FREE      (1 << 0)
#define BLOCK_METADATA  (1 << 1) // if set, the block will contain a node and then the data

// file permissions
// mode = user << 3 | kernel
#define MODE_X (1 << 0)
#define MODE_W (1 << 1)
#define MODE_R (1 << 2)

// fopen modes
#define FMODE_R (1 << 0)
#define FMODE_W (1 << 1)
#define FMODE_A (1 << 2)

#define MAX_PATH 256

typedef struct {
    uint32_t magic;

    uint8_t flags;
    uint64_t size; // for FS_DIR: size = n of children
    uint32_t first_block;
    unsigned mode : 3;

    char name[32];

    char reserved[78];
} __attribute__((packed)) node_t;


/*
    if a block contains the metadata of a directory (the node of a directory),
    the rest 3,960 bytes will contain the indices of the directory contents
*/
typedef struct {
    /*
        a uint32_t as a block index allows for up to 16 tb of mapped space (for 4096 block size)
        a uint64_t allows for up to 4096 eb
    */
    uint32_t next;
    unsigned attributes : 2;

    /*
        for FS_DIR these bytes will NOT be used. with this padding, the number of
        child indices is an integer for any BLOCK_SIZE % 4096 = 0
    */
    char padding[3];

    char data[BLOCK_SIZE - 4 - 1 - 3]; // 1 block is exactly 4 kib, including the metadata.
} __attribute__((packed)) block_t;

typedef struct {
    char magic[8];
    uint64_t size;
    uint16_t block_size;

    char reserved[46];
} __attribute__((packed)) fs_header_t;

typedef struct {
    char name[32];

    uint8_t is_locked;

    uint32_t start;
    uint64_t size;
    uint32_t ptr_local; // offset inside the file
    uint64_t ptr_global; // offset inside the drive

    uint8_t mode : 6;
    uint8_t open_mode : 3;
} file_t;

node_t *mknode(FILE *fp, char *path, int type);
file_t *_fopen(FILE *fp, char *path, uint8_t mode);
