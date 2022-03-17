/**
 * It allows you to use functions that are not part of the standard C library 
 * but are part of the POSIX.1 (IEEE Standard 1003.1) standard. Using the macros
 * described in feature_test_macros allows you to control the definitions 
 * exposed by the system header files.
 * https://man7.org/linux/man-pages/man7/feature_test_macros.7.html
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>    /* size_t */
#include <fcntl.h>    /* open(), read(), write() and their friends */
#include <sys/mman.h> /* mmap() */
#include <string.h>   /* strdup(), strncpy() */

/**
 * mkfs creates a FS image, with block size being 512 bytes. The size of the 
 * image depends on user input.
 * The first thing I noticed in this code is - there is no malloc(); instead,
 * there are ftruncate() and mmap().
 */


/**
 * Below is an illustration of the layout of the disk blocks:
 * |super block|       fat      |regular disk blocks|
 * |  block 0  | several blocks |  the rest blocks  |
 * 
 * superblock:
 *      It represents the data in the 1st disk block.
 *      It does not physically reside in 1st disk block; it is just a management
 *          structure that represents the meta data in this FS image.
 *      super->magic = "F439"; length 4.
 *      super->nBlocks = total blocks of this disk image;
 *      super->avail = super->nBlocks - 1;
 *      super->root = the index of the disk block;
 * 1st disk block:
 *      512 bytes; 
 *      the first 4 bytes: 2, 
 *      the next 4 bytes: size of file names (each name length is 16 bytes max)
 *      the rest bytes (beyond the 8 bytes): 
 *          is an array of 16 bytes elements, each element is:
 *          [fileName , starting disk block index]
 *          | 12 bytes|          4 bytes         |
 * 
 * fat: file allocation table
 *      It starts from the 2nd disk block.
 *      It might take up more than one disk blocks.
 *      It is initialized (in main) to be several leading zeros, and then
 *          fat[i] = i-1
 * 2nd (and next several) disk block(s):
 *      stores the fat, #of blocks depends on how many blocks in total.
 * 
 * The rest disk blocks:
 *      stores the actual files.
 *      For each file, it can take up 1 or more disk blocks. 
 *      The first disk block will be divided into 2 parts - 
 *          fileMetaData (8 bytes), 
 *              fileMetaData[0] = 1; fileMetaData[1] = sizeof file in bytes.
 *          then the actual data (512 - 8 bytes).
 *      The rest of the disk blocks associated with this file will just contain 
 *      the file data.
 */


/* super block that stores the information of this FS image */
typedef struct {
    char magic[4];
    /* below are all of type uint32_t, so they are all numbers */
    uint32_t nBlocks; /* the total number of blocks (n) */
    uint32_t avail;   /* the number of available blocks (n-1) */
    uint32_t root;    /* index of the root directory */
} Super;


/**
 * mapStart == super == blocks
 * mapStart: void * (for mmap())
 * super:    Super * - for super block 
 * blocks:   char * - 1 byte granularity
 * fat:      uint32_t * - starts from the second disk block, 4 byte granularity
 * 
 */
Super *super;  /* the super block that manages all the disk blocks; resides in the 1st disk block */
uint32_t *fat; /* file allocation table, an array of uint32_t */
char *blocks;  /* the disk blocks with 1 byte granularity */
void *mapStart; /* the starting of the mmap'ed area of the file system (disk image) */
size_t mapLength; /* the size of the disk image */


/**
 * @brief get the @index of the first available block;
 *        update @super->avail field;
 *        mark fat[@index] as 0, indicating that block is used.
 * @return the @index of the first available block.
 */
uint32_t getBlock() {
    /* get the index of the available disk block */
    uint32_t idx = super->avail;

    /* index starts from max avail, then decrease */
    if (idx == 0) {
        fprintf(stderr, "disk is full\n");
        exit(-1);
    }

    /* we update the @super->avail value, get one block from fat, and mark that 
       entry in fat as 0 */
    super->avail = fat[idx];
    fat[idx] = 0;
    return idx;
}

/**
 * @brief given an index of the disk block and the offset within the block,
 *        returns that address
 * 
 * @param idx index of the disk blocks, disk block size is 512 bytes
 * @param offset the offset within the disk block
 * @return the address within the disk block
 */
char *toPtr(uint32_t idx, uint32_t offset) {
    /* blocks is of type char * */
    return blocks + idx * 512 + offset;
}

/**
 * @brief given a file (in main(), the file is passed in as a parameter), we
 *        read the file, and store the file into our disk image. Since the disk
 *        block size is 512 bytes, and the file size may exceed the block size,
 *        we have to store the file into several disk blocks.
 * 
 * After we have read the file into our FS, we can use fat array to track how
 * many disk blocks each file takes up.
 * 
 * Suppose there are 6 disk blocks in total
 * fat initially was: [0,0,0,2,3,4]
 * If fat = [0,0,0,2,3,0], then there will be  
 * 
 * @param fileName the name of the file passed in with main().
 * @return the index of the disk block that stores the beginning of the file.
 */
uint32_t oneFile(const char *fileName) {
    int fd = open(fileName, O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(-1);
    }

    /* get the index within the disk blocks that has a free block */
    uint32_t startBlockIndex = getBlock();

    /* the offset passed into toPtr() is 0, so fileMetaData points to the start
       of the free disk block */
    uint32_t *fileMetaData = (uint32_t)toPtr(startBlockIndex, 0);

    /* disk block size is 512 bytes, the first 4 bytes stores 1 (metadata) */
    fileMetaData[0] = 1;

    uint32_t currentBlockIndex = startBlockIndex;

    /* this might be confusing - why minus 8 bytes? Because fileMetaData takes
       up 8 bytes and we have only written 4 bytes - 4 bytes not yet written.
       fileMetaData[0] = 1, fileMetaData[1] = file size */
    uint32_t leftInBlock = 512 - 8;
    uint32_t blockOffset = 8; /* the current offset within the block */
    uint32_t totalSize = 0; /* the size of the whole file */

    while (1) {
        /* if the block is full, then we need to get another disk block to store
           the rest of the file */
        if (leftInBlock == 0) {
            uint32_t b = getBlock(); /* get the index of the free disk block */
            // fat[currentBlockIndex] is 0, we update it to be currentBlockIndex
            fat[currentBlockIndex] = b;
            currentBlockIndex = b;
            blockOffset = 0;
            leftInBlock = 512;
        }

        /* read the file of length=leftInBlock to our FS disk block(s) */
        ssize_t n = read(fd, toPtr(currentBlockIndex, blockOffset), leftInBlock);
        if (n < 0) { 
            /* read failure */
            perror("read");
            exit(-1);
        } else if (n == 0) { 
            /* no more to read, file has reached EOF */
            fileMetaData[1] = totalSize;
            break;
        } else {
            /* update the tracking values */
            blockOffset += n;
            leftInBlock -= n;
            totalSize += n;
        }
    }

    close(fd);
    return startBlockIndex;
}



int main(int argc, const char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <image name> <nBlocks> <file0> <file1> ...\n", argv[0]);
        exit(1);
    }

    const char *imageName = argv[1];   /* name of the image */
    int nBlocks = atoi(argv[2]);       /* number of blocks for FS */
    const char **fileNames = &argv[3]; /* treat file names as an array */
    int nFiles = argc - 3;             /* number of the files in this image */

    /* open the image, if not exist, then create one */
    /* 0777: user, group, others all have read(4), write(2) and execute(1) permission
       0666 = S_IRUSR | S_IWUSR | // user has read(00400) and write(00200) permission
              S_IRGRP | S_IWGRP | // group has read(00040) and write(00020) permission 
              S_IROTH | S_IWOTH   // others have read(00004) and write(00002) permission
    */
    int fd = open(imageName, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("create"); /* perror prints a system error msg */
        exit(1);
    }

    mapLength = nBlocks * 512;

    /* truncate the image to length of (nBlocks * 512) */
    int rc = ftruncate(fd, mapLength);
    if (rc == -1) {
        perror("truncate");
        exit(1);
    }

    /* map current process to memory, with length being (nBlocks * 512) */
    mapStart = mmap(0, mapLength, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapStart == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    /* assign the super block to the start of the mmap'ed area (1st disk block) */
    super = (Super *)mapStart;
    /* assign the blocks to the start of the mmap'ed area as well */
    blocks = (char *)mapStart;

    /* file allocation table starts from the second disk block (block size 512) */
    fat = (uint32_t *)(blocks + 512);

    /* fatBlocks is the number of the disk blocks that fat itself takes up. */
    uint32_t fatBlocks = (nBlocks * sizeof(uint32_t) + 511) / 512;

    /* fill in the super block - 1st disk block */
    super->magic[0] = 'F';
    super->magic[1] = '4';
    super->magic[2] = '3';
    super->magic[3] = '9';
    super->nBlocks = nBlocks;
    super->avail = nBlocks - 1; /* super block takes up the 1st block */

    /* Below is the initialization of fat. Consider a simple example:
       Say fat takes up 2 whole disk blocks, i.e. @fatBlocks is 2.
       Since a disk block size is 512, each fat entry is 32-bit, 4 bytes, we
       have @nBlocks = (512/4)*2 = 256.
       Since super block takes up one block, fat takes up 2, there will be 253
       remaining disk blocks for us to store things.
            @nBlocks = 256;
            @fatBlocks=2 -> @lastAvail = 3; 
            @super->avail = nBlocks-1 = 255;
       
       
        So fat = [_,_,_,_,3,...,253,254]  <-- the actual data in fat
          index:  0,1,2,3,4,...,254,255   <-- the index
        index 0 stores super block, index 1, 2 store fat.
    */
    uint32_t lastAvail = 1 + fatBlocks;

    for (uint32_t i = super->avail; i > lastAvail; i--) {
        fat[i] = i - 1;
    }

    /* request one block for root directory (superblock->root) */
    super->root = getBlock();
    uint32_t *rootMetaData = (uint32_t *)toPtr(super->root, 0);
    rootMetaData[0] = 2; /* root has inode number 2 */
    rootMetaData[1] = nFiles * 16; /* rootMetaData[] takes up 8 bytes;
                                      file name total size is rootMetaData[1] */
    /* @rootData points to offset 8 within the 1st disk block.
       it serves as an array of fileName entries (16 bytes each) */
    char *rootData = toPtr(super->root, 8);

    /* iterate over files */
    for (int i = 0; i < nFiles; i++) {
        uint32_t x = oneFile(fileNames[i]);
        char *nm = strdup(fileNames[i]); /* duplicate a string, with malloc() */
        char *base = basename(nm); /* get the name with leading directory components removed */
        char *dest = strncpy(rootData + i * 16, base, 12);
        free(nm); /* strdup() internally calls malloc(), so need to free() */
        
        /* after we have copied file name, needs to write the starting disk
           block index. Note that each fileName entry is 16 bytes*/
        *((uint32_t *)(dest + 12)) = x; 
    }

    munmap(mapStart, mapLength);

    return 0;
}