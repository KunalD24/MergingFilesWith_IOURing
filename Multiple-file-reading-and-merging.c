#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>

#define QD  64
#define BS  4096

struct file_info{
    int fd;
    off_t offset;
    char *buffer;
    size_t size;
    int eof;
    const char *name;
};

int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr,"Usage : %s output_file input1 input2....\n",argv[0]);
        return 1;
    }

    const char *outname = argv[1];
    int file_count = argc - 2;
    
    int outfd = open(outname, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if(outfd < 0){
        perror("open");
        return 1;
    }

    struct file_info *files = calloc(file_count, sizeof(*files));
    if (!files) {
        perror("calloc");
        return 1;
    }

    for (int i = 0; i < file_count; i++) {
        files[i].fd = open(argv[i + 2], O_RDONLY);
        if (files[i].fd < 0) {
            perror("open input");
            return 1;
        }
        files[i].offset = 0;
        files[i].size = BS;
        files[i].buffer = malloc(BS);
        files[i].eof = 0;
        files[i].name = argv[i + 2];
    }
    
    struct io_uring ring;
    if (io_uring_queue_init(QD, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        return 1;
    }

    for (int i = 0; i < file_count; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_read(sqe, files[i].fd, files[i].buffer, BS, 0);
        io_uring_sqe_set_data(sqe, &files[i]);
    }
    io_uring_submit(&ring);

    int completed = 0;
    while (completed < file_count) {
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            perror("io_uring_wait_cqe");
            break;
        }

        struct file_info *fi = io_uring_cqe_get_data(cqe);

        if (cqe->res < 0 || cqe->res == 0) {
            fi->eof = 1;
            completed++;
        } else {
            if (fi->offset == 0 && fi != &files[0]) {
                write(outfd, "\n\n", 2);
            }

            if (write(outfd, fi->buffer, cqe->res) != cqe->res) {
                perror("write output");
                return 1;
            }

            fi->offset += cqe->res;

            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, fi->fd, fi->buffer, BS, fi->offset);
            io_uring_sqe_set_data(sqe, fi);
            io_uring_submit(&ring);
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    for (int i = 0; i < file_count; i++) {
        close(files[i].fd);
        free(files[i].buffer);
    }
    free(files);
    close(outfd);
    io_uring_queue_exit(&ring);

    return 0;
}