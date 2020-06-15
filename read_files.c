#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "liburing.h"
#include "liburing/io_uring.h"

/*
 * Read a number of files in parallel using io_uring
 *
 * g++ -Wall -O2 -o read_files read_files.c -luring
 *
 * -- Steps --
 * 1. Create the ring
 * 2. Check whether non-vectored read is supported using io_uring_probe
 * 3. If so, submit read(v) operations
 * 4. Reap read-completed completion queue entries
 * 5. Tear-down
 *
 */

struct RIOVec {
    const char *pathname;
    int fd;
    // fields in ROOT data structure
    void *fBuffer;
    off_t fOffset;
    size_t fSize;
    size_t fOutBytes;
};

// caller responsible for freeing using free_read_data
static int make_riovec(const char *pathname, RIOVec *rd) {
    rd->pathname = pathname;
    rd->fd = open(pathname, O_RDONLY);
    if (rd->fd < 0) {
        perror("open");
        return 1;
    }
    struct stat st;
    if (fstat(rd->fd, &st)) {
        perror("fstat");
        return 1;
    }
    rd->fBuffer = malloc(st.st_size);
    if (!rd->fBuffer) {
        perror("malloc");
        return 1;
    }
    rd->fOffset = 0; // read whole file
    rd->fSize = st.st_size;
    rd->fOutBytes = 0; // set by cqe
    return 0;
}

void free_riovec(RIOVec *io) {
    if (NULL != io->fBuffer) {
        free(io->fBuffer);
    }
}

// io_uring demo

static int prep_reads(struct io_uring *ring, RIOVec files[], int num_files) {
    struct io_uring_sqe *sqe;
    for (int i = 0; i < num_files; i++) {
        sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            fprintf(stderr, "sqe get failed\n");
            return 1;
        }

        io_uring_prep_read(sqe,
            files[i].fd,
            files[i].fBuffer,
            files[i].fSize,
            files[i].fOffset
        );

        // mark position in files array
        sqe->user_data = i;
    }
    return 0;
}

static int reap_reads(struct io_uring *ring, RIOVec files[], int num_files) {
    struct io_uring_cqe *cqe;
    int ret;

    for (int i = 0; i < num_files; i++) {
        // if right number of sqes are submitted, won't hang but may return failure cqe
        // if too few sqes are submitted, could wait forever
        // -- maybe we switch to timeout
        ret = io_uring_wait_cqe(ring, &cqe);
        if (ret) {
            fprintf(stderr, "wait cqe: %d\n", ret);
            return 1;
        }
        unsigned long index = (unsigned long) io_uring_cqe_get_data(cqe);
        if (index < 0 || index >= (unsigned long)num_files) {
            fprintf(stderr, "bad cqe user_data: %lu\n", index);
            return 1;
        }
        if (cqe->res < 0) {
            fprintf(stderr, "read file[%lu] failed: %s\n", index, strerror(-cqe->res));
            return 1;
        }
        files[index].fOutBytes = (size_t)cqe->res;
        printf("read %lu bytes from file %lu\n", files[index].fOutBytes, index);

        // advance ring
        io_uring_cqe_seen(ring, cqe);
    }
    return 0;
}

int main(int argc, char* argv[]) {

    if (argc < 2) {
        printf("%s: file [files...]\n", argv[0]) ;
        return 1;
    }
    int num_files = argc - 1;
    printf("reading %d files\n", num_files);

    struct io_uring ring;
    struct io_uring_probe *p;
    int ret;

    // todo check whether queue size != num_files matters
    ret = io_uring_queue_init(num_files, &ring, 0 /* no setup flags */);
    if (ret) {
        fprintf(stderr, "ring create failed: %d\n", ret);
        return 1;
    }

    // kinda dumb, could fallback to readv with length one
    // -- keep read for simplicity
    p = io_uring_get_probe_ring(&ring);
    if (!p || !io_uring_opcode_supported(p, IORING_OP_READ)) {
        fprintf(stderr, "read op not supported by kernel, exiting: %d\n", ret);
        return 1;
    }
    free(p);

    RIOVec *files = (RIOVec*)calloc(num_files, sizeof(RIOVec));
    if (!files) {
        perror("calloc");
        return 1;
    }
    for (int i = 0; i < num_files; i++) {
        int err = make_riovec(argv[i+1], &files[i]);
        if (err) {
            fprintf(stderr, "initialization failed for file[%d] (%s)\n", i, argv[i+1]);
            return 1;
        }
    }

    ret = prep_reads(&ring, files, num_files);
    if (ret) {
        fprintf(stderr, "prep reads failed: %d\n", ret);
        return 1;
    }

    ret = io_uring_submit(&ring);
    if (ret <= 0) {
        fprintf(stderr, "submit sqe failed: %d\n", ret);
        return 1;
    }
    printf("submitted %d sqes\n", ret);

    ret = reap_reads(&ring, files, num_files);
    if (ret) {
        fprintf(stderr, "reap reads failed: %d\n", ret);
        return 1;
    }

    io_uring_queue_exit(&ring);

    for (int i = 0; i < num_files; i++) {
        free_riovec(&files[i]);
    }
    free(files);
    return 0;
}
