#include "fs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


char **split_string(const char *str, const char *delimiter, int *num_tokens) {
    char *str_copy, *token, *saveptr;
    int count = 0;
    char **result = NULL;
    str_copy = strdup(str);

    // count the tokens to allocate memory
    token = strtok_r(str_copy, delimiter, &saveptr);
    while (token != NULL) {
        count++;
        token = strtok_r(NULL, delimiter, &saveptr);
    }

    result = (char **) malloc(count * sizeof(char*));
    
    // extract the tokens
    strcpy(str_copy, str);
    token = strtok_r(str_copy, delimiter, &saveptr);
    count = 0;
    while (token != NULL) {
        result[count] = strdup(token);
        count++;
        token = strtok_r(NULL, delimiter, &saveptr);
    }

    free(str_copy);
    *num_tokens = count;
    return result;
}

uint8_t mode_calc(uint8_t user, uint8_t kernel) {
    return user << 3 | kernel;
}

void new_fs(FILE *fp, uint64_t size, int isos) {
    if (size % BLOCK_SIZE != 0) {
        printf("fs size must be a multiple of %d kib", BLOCK_SIZE / 1024);
        return;
    }

    block_t *blk = malloc(BLOCK_SIZE);
    blk->next = 0;
    blk->attributes |= BLOCK_METADATA;

    fs_header_t *fs = malloc(sizeof(fs_header_t));
    strcpy(fs->magic, FS_MAGIC);
    fs->size = size;
    fs->block_size = BLOCK_SIZE;
    
    memcpy(blk->data, fs, sizeof(fs_header_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);

    blk->next = 0;
    blk->attributes |= BLOCK_METADATA;
    memset(blk->data, 0, sizeof(blk->data));

    node_t *node = malloc(sizeof(node_t));
    node->first_block = 1;
    node->flags |= FS_DIR;
    node->magic = NODE_MAGIC;
    node->size = 0;
    node->mode = mode_calc(MODE_R, MODE_R | MODE_W);
    strcpy(node->name, "ROOT");

    memcpy(blk->data, node, sizeof(node_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);

    blk->next = 0;
    blk->attributes = 0;
    blk->attributes |= BLOCK_FREE;
    memset(blk->data, 0, sizeof(blk->data));

    for (uint32_t i = 2 * BLOCK_SIZE; i < size; i+=BLOCK_SIZE) {
        fwrite(blk, BLOCK_SIZE, 1, fp);
    }

    mknode(fp, "/dev", FS_DIR);
    mknode(fp, "/dev/stdout", FS_CHARDEV);
    mknode(fp, "/dev/stdin", FS_CHARDEV);
}

/*
    finds the next free block in the filesystem.
    returns null if there are no free blocks available

    iblk: starting block
*/
static uint32_t find_next_free_block(FILE *fp, block_t *dest, uint32_t iblk) {
    uint64_t offset = iblk * BLOCK_SIZE;
    uint32_t fs_size = 1048576;

    block_t *blk = malloc(BLOCK_SIZE);
    if (blk == NULL) printf("n\n");
    fseek(fp, offset, SEEK_SET);

    while (offset < fs_size) {
        memset(blk, 0, BLOCK_SIZE);
        fread(blk, BLOCK_SIZE, 1, fp);

        if (blk->attributes & BLOCK_FREE) {
            if (dest != NULL) {
                memcpy(dest, blk, BLOCK_SIZE);
            }
            free(blk);
            return offset / BLOCK_SIZE;
        }

        offset += BLOCK_SIZE;
    }

    free(blk);
    return 0;
}

static block_t *get_block_by_index(FILE *fp, uint32_t iblk) {
    uint64_t offset = iblk * BLOCK_SIZE;
    block_t *blk = malloc(BLOCK_SIZE);

    fseek(fp, offset, SEEK_SET);
    fread(blk, BLOCK_SIZE, 1, fp);

    return blk;
}

static uint32_t find_node(FILE *fp, char *path, int type, int parent, node_t *dst) {
    int nsplits = 0;
    char **pwd_split = split_string(path, "/", &nsplits);
    if (parent) nsplits--;

    block_t *blk = get_block_by_index(fp, 1);
    node_t *node = malloc(sizeof(node_t));

    memcpy(node, blk->data, sizeof(node_t));
    uint32_t *children = (uint32_t *) (blk->data + sizeof(node_t));

    int found = 0;
    uint32_t block_offset = 1;
    node_t *temp = malloc(sizeof(node_t));
    block_t *b;
    for (int i = 0; i < nsplits; i++) {
        found = 0;
        for (int j = 0; j < node->size; j++) { // iterate through each subdirectory of the parent.
            // possible data corruption
            if (*children == 0) return 0;

            b = get_block_by_index(fp, *children);
            assert(b->attributes & BLOCK_METADATA); // block has to contain a node
            memcpy(temp, b->data, sizeof(node_t));
            assert(temp->magic == NODE_MAGIC);

            int target_type = (i == nsplits-1) ? type : FS_DIR;
            if (temp->flags & target_type) {
                if (strcmp(temp->name, pwd_split[i]) == 0) {
                    found = 1;
                    block_offset = *children;
                    memcpy(node, temp, sizeof(node_t));
                    memcpy(blk, b, BLOCK_SIZE);

                    children = (uint32_t *) (blk->data + sizeof(node_t));
                    free(b);
                    break;
                }
            }

            children++;
            free(b);
        }

        // node was not found.
        if (found == 0) {
            return 0;
        }
    }

    if (parent) nsplits++;
    for (int i = 0; i < nsplits; i++) {
        free(pwd_split[i]);
    }

    memcpy(dst, node, sizeof(node_t));
    free(node);
    free(temp);
    free(pwd_split);

    return block_offset;
}

node_t *mknode(FILE *fp, char *path, int type) {
    node_t *node = malloc(sizeof(node_t));
    uint32_t block_offset = find_node(fp, path, FS_DIR, 1, node);
    if (block_offset == 0) {
        free(node);
        return NULL;
    }

    int nsplits = 0;
    char **temp = split_string(path, "/", &nsplits);
    char *name = malloc(32);
    strcpy(name, temp[nsplits-1]);

    for (int i = 0; i < nsplits; i++) {
        free(temp[i]);
    }
    free(temp);

    block_t *blk = get_block_by_index(fp, block_offset);
    memcpy(node, blk->data, sizeof(node_t));

    uint32_t parent_size = node->size;
    uint32_t parent_off = node->first_block;

    block_t *block = malloc(BLOCK_SIZE);
    uint32_t r = find_next_free_block(fp, block, block_offset);
    assert(r > 0);

    fseek(fp, parent_off * BLOCK_SIZE, SEEK_SET);

    memcpy(blk->data + sizeof(node_t) + node->size * 4, &r, 4);
    node->size++;
    memcpy(blk->data, node, sizeof(node_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);
    fseek(fp, r * BLOCK_SIZE, SEEK_SET);
    
    memset(blk, 0, BLOCK_SIZE);
    memset(node, 0, sizeof(node_t));

    node->magic = NODE_MAGIC;
    node->first_block = r;
    node->flags |= type;
    if (type == FS_FILE) node->mode = 63; // rwx rwx
    else if (type == FS_DIR) node->mode = 54; // rw- rw-
    else if (type == FS_CHARDEV) node->mode = 38; // r-- rw-
    node->size = 0;
    strcpy(node->name, name);

    blk->attributes |= BLOCK_METADATA;
    blk->next = 0;
    memcpy(blk->data, node, sizeof(node_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);

    free(blk);
    return node;
}

// if both append and write is selected, write will be executed
file_t *_fopen(FILE *fp, char *path, uint8_t mode) {
    if (mode == 0) return NULL;

    node_t *node = malloc(sizeof(node_t));
    uint32_t off = find_node(fp, path, FS_FILE, 0, node);
    file_t *file = malloc(sizeof(file_t));

    if (mode & FMODE_W) {
        if (off == 0) { // file does not exist
            free(node); // free old node
            node = mknode(fp, path, FS_FILE);
        }

        if ((node->mode & MODE_W) == 0) {
            free(node);
            free(file);
            return NULL; // not writeable
        }

        file->node = node;
        file->mode = mode;
        file->is_locked = 0;
        file->ptr_local = 0;
        file->ptr_global = BLOCK_SIZE * node->first_block;
    } else if (mode & FMODE_A) { // append
        if (off == 0) { // file does not exist
            free(node);
            node = mknode(fp, path, FS_FILE);
        }

        if ((node->mode & MODE_W) == 0) {
            free(node);
            free(file);
            return NULL;
        }

        // we need to find the last block to set the file pointer
        block_t *block = get_block_by_index(fp, node->first_block);
        int next = node->first_block;
        while (block->next != 0) {
            next = block->next;
            free(block);

            block = get_block_by_index(fp, block->next);
        }

        file->node = node;
        file->mode = mode;
        file->is_locked = 0;
        file->ptr_local = node->size;
        file->ptr_global = next * BLOCK_SIZE + node->size % BLOCK_SIZE;
        if (next == node->first_block) file->ptr_global += sizeof(node_t);
    } else if (mode & FMODE_R) {
        if (off == 0) {
            free(node);
            free(file);
            return NULL;
        }

        file->node = node;
        file->mode = mode;
        file->is_locked = 0;
        file->ptr_local = 0;
        file->ptr_global = BLOCK_SIZE * node->first_block;
    }

    return file;
}

int _fread(FILE *fp, file_t *file, uint32_t size, uint8_t *buffer) {
    if ((file->mode & FMODE_R) == 0) {
        return -1;
    }

    // wait for the file to be unlocked
    while (file->is_locked != 0);

    file->is_locked = 1;

    // in the first block of the file there's both metadata and data
    int offset = file->ptr_local + sizeof(node_t);

    int remaining_size = (file->node->size + sizeof(node_t)) - offset;
    if (remaining_size <= 0) {
        return -1; // invalid seek/reached EOF
    } else if (remaining_size < size) {
        size = remaining_size;
    }

    // find seek pointer in current block.
    int nblocks_real = offset / BLOCK_DATA_SIZE; // get number of real blocks.
    int off_crt_block = offset - nblocks_real * BLOCK_DATA_SIZE;

    block_t *crt_block = get_block_by_index(fp, file->ptr_global / BLOCK_SIZE);

    // read blocks
    // remaining data = BLOCK_DATA_SIZE - off_crt_block
    if (size <= BLOCK_DATA_SIZE - off_crt_block) {
        memcpy(buffer, crt_block->data + off_crt_block, size);

        file->ptr_local += size;
        file->ptr_global += size;
        return 0;
    }

    // size > current block
    memcpy(buffer, crt_block->data + off_crt_block, BLOCK_DATA_SIZE - off_crt_block);
    buffer += BLOCK_DATA_SIZE - off_crt_block;
    size -= BLOCK_DATA_SIZE - off_crt_block;

    int temp = size / BLOCK_DATA_SIZE; // number of whole blocks to read
    int next = crt_block->next;
    for (int i = 0; i < temp; i++) {
        if (crt_block->next == 0) {
            return -1; // I/O error
        }

        // find next block
        free(crt_block);
        crt_block = get_block_by_index(fp, next);
        memcpy(buffer, crt_block->data, BLOCK_DATA_SIZE);


        next = crt_block->next;
        buffer += BLOCK_DATA_SIZE;
        size -= BLOCK_DATA_SIZE;
        file->ptr_local += BLOCK_DATA_SIZE;
    }

    // we do not need the entire block.
    if (size > 0) {
        if (crt_block->next == 0) {
            return -1; // I/O error
        }

        int next = crt_block->next;
        free(crt_block);
        crt_block = get_block_by_index(fp, next);
        memcpy(buffer, crt_block->data, size);
    }

    file->ptr_global = BLOCK_SIZE * next + size;
    file->ptr_local += size;
    file->is_locked = 0;

    free(crt_block);
    return 0;
}

int _fwrite(FILE *fp, file_t *file, uint32_t size, uint8_t *buffer) {
    if ((file->mode & FMODE_W) == 0) { // append mode is not implemented.
        return -1; // file is readonly
    }

    if (size == 0) return 0;

    while (file->is_locked != 0);
    file->is_locked = 1;

    int offset = file->ptr_local + sizeof(node_t);
    int nblocks_real = offset / BLOCK_DATA_SIZE; // get number of real blocks.
    int off_crt_block = offset - nblocks_real * BLOCK_DATA_SIZE;

    block_t *crt_block = get_block_by_index(fp, file->ptr_global / BLOCK_SIZE);

    // set new size
    if (file->ptr_local + size > file->node->size) {
        file->node->size += file->ptr_local + size - file->node->size;
    }

    // update node size
    if (file->ptr_global / BLOCK_SIZE == file->node->first_block) {
        // current block is the first one
        memcpy(crt_block->data, file->node, sizeof(node_t));
    } else {
        // find first block
        block_t *first_blk = get_block_by_index(fp, file->node->first_block);
        memcpy(first_blk->data, file->node, sizeof(node_t));

        fwrite(first_blk, BLOCK_SIZE, 1, fp);
        free(first_blk);
    }

    // write single block
    if (size <= BLOCK_DATA_SIZE - off_crt_block) {
        memcpy(crt_block->data + off_crt_block, buffer, size);

        fseek(fp, (file->ptr_global / BLOCK_SIZE) * BLOCK_SIZE, SEEK_SET);
        int i = fwrite(crt_block, BLOCK_SIZE, 1, fp);

        file->ptr_global += size;
        file->ptr_local += size;
        
        free(crt_block);

        file->is_locked = 0;
        return 0;
    }

    // first block
    memcpy(crt_block->data + off_crt_block, buffer, BLOCK_DATA_SIZE - off_crt_block);

    // for efficiency reasons, all the blocks that belong to a file will be placed after the previous file block.
    uint32_t next_idx = find_next_free_block(fp, NULL, file->ptr_global / BLOCK_SIZE);
    crt_block->next = next_idx;

    fseek(fp, (file->ptr_global / BLOCK_SIZE) * BLOCK_SIZE, SEEK_SET);
    fwrite((uint8_t *) crt_block, BLOCK_SIZE, 1, fp);

    fseek(fp, next_idx * BLOCK_SIZE + 4, SEEK_SET);
    char buff[1];
    buff[0] = 0;
    fwrite(buff, 1, 1, fp); // set block as not free, so its not detected by find_next_free_block

    free(crt_block);

    buffer += BLOCK_DATA_SIZE - off_crt_block;
    size -= BLOCK_DATA_SIZE - off_crt_block;

    int c = 0;
    int temp = size / BLOCK_DATA_SIZE;
    for (int i = 0; i < temp; i++) {
        crt_block = get_block_by_index(fp, next_idx);

        c = next_idx; // save current index to calculate offset later.
        next_idx = find_next_free_block(fp, NULL, next_idx);

        crt_block->attributes = 0; // remove BLOCK_FREE flag.
        crt_block->next = (size > 0) ? next_idx : 0;
        memcpy(crt_block->data, buffer, BLOCK_DATA_SIZE);

        fseek(fp, c * BLOCK_SIZE, SEEK_SET);
        fwrite(crt_block, BLOCK_SIZE, 1, fp);

        fseek(fp, next_idx * BLOCK_SIZE + 4, SEEK_SET);
        fwrite(buff, 1, 1, fp); // mark next block as not free.

        buffer += BLOCK_DATA_SIZE;
        size -= BLOCK_DATA_SIZE;

        free(crt_block);
    }

    // write last block
    if (size > 0) {
        crt_block = get_block_by_index(fp, next_idx);

        crt_block->attributes = 0;
        crt_block->next = 0;
        memcpy(crt_block->data, buffer, size);

        fseek(fp, next_idx * BLOCK_SIZE, SEEK_SET);
        fwrite(crt_block, BLOCK_SIZE, 1, fp);
    }

    file->is_locked = 0;
    return 0;
}
