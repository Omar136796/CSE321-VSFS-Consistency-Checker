#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// Constants
#define BLOCK_SIZE 4096
#define TOTAL_BLOCKS 64
#define INODE_SIZE 256
#define INODE_COUNT 80  // 5 blocks * 4096 bytes / 256 bytes per inode
#define MAGIC_NUMBER 0xD34D
#define DATA_BLOCKS_START 8
#define DATA_BLOCKS_COUNT 56

// Superblock structure
typedef struct {
    uint16_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_bitmap_block;
    uint32_t data_bitmap_block;
    uint32_t inode_table_start;
    uint32_t first_data_block;
    uint32_t inode_size;
    uint32_t inode_count;
    uint8_t reserved[4058];
} Superblock;

// Inode structure
typedef struct {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint32_t links;
    uint32_t blocks;
    uint32_t direct[1];  // We only use one direct block for this simple FS
    uint32_t single_indirect;
    uint32_t double_indirect;
    uint32_t triple_indirect;
    uint8_t reserved[156];
} Inode;

// Global variables
uint8_t* fs_image = NULL;
Superblock sb;
uint8_t inode_bitmap[BLOCK_SIZE];
uint8_t data_bitmap[BLOCK_SIZE];
Inode inodes[INODE_COUNT];
uint32_t block_refcount[TOTAL_BLOCKS] = {0};
int errors_found = 0;

// Helper functions
void read_superblock() {
    memcpy(&sb, fs_image, sizeof(Superblock));
}

void read_inode_bitmap() {
    memcpy(inode_bitmap, fs_image + sb.inode_bitmap_block * BLOCK_SIZE, BLOCK_SIZE);
}

void read_data_bitmap() {
    memcpy(data_bitmap, fs_image + sb.data_bitmap_block * BLOCK_SIZE, BLOCK_SIZE);
}

void read_inodes() {
    for (int i = 0; i < INODE_COUNT; i++) {
        uint32_t offset = sb.inode_table_start * BLOCK_SIZE + i * INODE_SIZE;
        memcpy(&inodes[i], fs_image + offset, sizeof(Inode));
    }
}

int is_inode_allocated(int inode_num) {
    if (inode_num < 0 || inode_num >= INODE_COUNT) return 0;
    return (inode_bitmap[inode_num / 8] >> (inode_num % 8)) & 1;
}

int is_block_allocated(int block_num) {
    if (block_num < 0 || block_num >= TOTAL_BLOCKS) return 0;
    return (data_bitmap[block_num / 8] >> (block_num % 8)) & 1;
}

void mark_block_referenced(int block_num) {
    if (block_num >= 0 && block_num < TOTAL_BLOCKS) {
        block_refcount[block_num]++;
    }
}

// Validation functions
void validate_superblock() {
    printf("Validating superblock...\n");
    
    if (sb.magic != MAGIC_NUMBER) {
        printf("ERROR: Invalid magic number (0x%04X, expected 0x%04X)\n", sb.magic, MAGIC_NUMBER);
        errors_found++;
    }
    
    if (sb.block_size != BLOCK_SIZE) {
        printf("ERROR: Invalid block size (%u, expected %u)\n", sb.block_size, BLOCK_SIZE);
        errors_found++;
    }
    
    if (sb.total_blocks != TOTAL_BLOCKS) {
        printf("ERROR: Invalid total blocks (%u, expected %u)\n", sb.total_blocks, TOTAL_BLOCKS);
        errors_found++;
    }
    
    if (sb.inode_bitmap_block != 1) {
        printf("ERROR: Invalid inode bitmap block (%u, expected 1)\n", sb.inode_bitmap_block);
        errors_found++;
    }
    
    if (sb.data_bitmap_block != 2) {
        printf("ERROR: Invalid data bitmap block (%u, expected 2)\n", sb.data_bitmap_block);
        errors_found++;
    }
    
    if (sb.inode_table_start != 3) {
        printf("ERROR: Invalid inode table start block (%u, expected 3)\n", sb.inode_table_start);
        errors_found++;
    }
    
    if (sb.first_data_block != DATA_BLOCKS_START) {
        printf("ERROR: Invalid first data block (%u, expected %d)\n", sb.first_data_block, DATA_BLOCKS_START);
        errors_found++;
    }
    
    if (sb.inode_size != INODE_SIZE) {
        printf("ERROR: Invalid inode size (%u, expected %d)\n", sb.inode_size, INODE_SIZE);
        errors_found++;
    }
    
    if (sb.inode_count != INODE_COUNT) {
        printf("ERROR: Invalid inode count (%u, expected %d)\n", sb.inode_count, INODE_COUNT);
        errors_found++;
    }
}

void validate_inode_bitmap() {
    printf("Validating inode bitmap...\n");
    
    for (int i = 0; i < INODE_COUNT; i++) {
        int allocated = is_inode_allocated(i);
        int valid = (inodes[i].links > 0) && (inodes[i].dtime == 0);
        
        if (allocated && !valid) {
            printf("ERROR: Inode %d is marked as allocated but is invalid (links=%u, dtime=%u)\n", 
                   i, inodes[i].links, inodes[i].dtime);
            errors_found++;
        }
        
        if (!allocated && valid) {
            printf("ERROR: Inode %d is valid but not marked in bitmap (links=%u, dtime=%u)\n", 
                   i, inodes[i].links, inodes[i].dtime);
            errors_found++;
        }
    }
}

void validate_data_bitmap() {
    printf("Validating data bitmap...\n");
    
    // First check all allocated blocks are referenced
    for (int block = DATA_BLOCKS_START; block < TOTAL_BLOCKS; block++) {
        if (is_block_allocated(block) && block_refcount[block] == 0) {
            printf("ERROR: Data block %d is marked as allocated but not referenced by any inode\n", block);
            errors_found++;
        }
    }
    
    // Then check all referenced blocks are allocated
    for (int block = 0; block < TOTAL_BLOCKS; block++) {
        if (block_refcount[block] > 0 && !is_block_allocated(block)) {
            printf("ERROR: Data block %d is referenced but not marked as allocated\n", block);
            errors_found++;
        }
    }
}

void validate_inodes() {
    printf("Validating inodes and block references...\n");
    
    for (int i = 0; i < INODE_COUNT; i++) {
        if (!is_inode_allocated(i)) continue;
        
        // Check direct blocks
        for (int j = 0; j < 1; j++) { // We only use one direct block in this simple FS
            uint32_t block = inodes[i].direct[j];
            if (block != 0) {
                if (block < DATA_BLOCKS_START || block >= TOTAL_BLOCKS) {
                    printf("ERROR: Inode %d references invalid block %u\n", i, block);
                    errors_found++;
                } else {
                    mark_block_referenced(block);
                }
            }
        }
        
        // Check indirect blocks (not used in this simple FS, but we check anyway)
        if (inodes[i].single_indirect != 0) {
            printf("ERROR: Inode %d has non-zero single indirect pointer (%u)\n", i, inodes[i].single_indirect);
            errors_found++;
        }
        
        if (inodes[i].double_indirect != 0) {
            printf("ERROR: Inode %d has non-zero double indirect pointer (%u)\n", i, inodes[i].double_indirect);
            errors_found++;
        }
        
        if (inodes[i].triple_indirect != 0) {
            printf("ERROR: Inode %d has non-zero triple indirect pointer (%u)\n", i, inodes[i].triple_indirect);
            errors_found++;
        }
    }
}

void check_duplicate_blocks() {
    printf("Checking for duplicate blocks...\n");
    
    for (int block = 0; block < TOTAL_BLOCKS; block++) {
        if (block_refcount[block] > 1) {
            printf("ERROR: Block %d is referenced by %d inodes\n", block, block_refcount[block]);
            errors_found++;
        }
    }
}

void print_summary() {
    printf("\nConsistency check completed.\n");
    if (errors_found == 0) {
        printf("No errors found. File system is consistent.\n");
    } else {
        printf("Found %d errors in the file system.\n", errors_found);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <filesystem_image>\n", argv[0]);
        return 1;
    }
    
    // Read the entire file system image into memory
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror("Failed to open file system image");
        return 1;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (size != BLOCK_SIZE * TOTAL_BLOCKS) {
        printf("ERROR: File size (%ld) doesn't match expected size (%d)\n", 
               size, BLOCK_SIZE * TOTAL_BLOCKS);
        fclose(fp);
        return 1;
    }
    
    fs_image = malloc(size);
    if (!fs_image) {
        perror("Failed to allocate memory");
        fclose(fp);
        return 1;
    }
    
    if (fread(fs_image, 1, size, fp) != size) {
        perror("Failed to read file system image");
        free(fs_image);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    
    // Read file system structures
    read_superblock();
    read_inode_bitmap();
    read_data_bitmap();
    read_inodes();
    
    // Run consistency checks
    validate_superblock();
    validate_inodes();
    validate_inode_bitmap();
    validate_data_bitmap();
    check_duplicate_blocks();
    
    // Print summary
    print_summary();
    
    free(fs_image);
    return errors_found ? 1 : 0;
}
