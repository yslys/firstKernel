#include <stdio.h>  /* size_t */
#include <unistd.h> /* write() */
#include <fcntl.h>  /* creat */


/**
 * create a file named big.data, and write to it. The contents are like:
 * 0123456789101112...29982999 
 */



void writeFully(int fd, void *buf, size_t length) {
    size_t togo = length;
    char *p = (char *)buf;
    while (togo) {
        /* write @togo bytes from @p to the file @fd */
        size_t cnt = write(fd, p, togo);
        if (cnt <= 0) {
            perror("write");
            exit(-1);
        }
        p += cnt; /* update the pointer to the data to write */
        togo -= cnt; /* update the remaining size to write */
    }
}


int main() {
    /* A call to creat() is equivalent to calling open() with flags
       equal to O_CREAT|O_WRONLY|O_TRUNC. For 2nd param, see mkfs.c
       https://man7.org/linux/man-pages/man2/creat.2.html */
    int fd = creat("big.data", 0666);
    if (fd < 0) {
        perror("open");
        exit(1);
    }

    /* write 0,1,2,...,2999 to the file big.data */
    for (int i = 0; i < 3000; i++) {
        writeFully(fd, &i, sizeof(int));
    }

    close(fd);
    return 0;
}