#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "liburing.h"
#include "liburing/io_uring.h"

/*
 * Read a number of files in parallel using io_uring
 *
 * g++ -Wall -O2 -o read_files read_files.c -luring
 */

// #define QUEUE_DEPTH 4

// class IoUring {
// private:
//     struct io_uring fRing;
// public:
//     IoUring() = delete;
//     IoUring(uint32_t size, struct io_uring_params *params) : fRing() {}
//     ~IoUring() {
//         io_uring_queue_exit(&fRing);
//     }
//
//     struct io_uring *ring() {
//         return &fRing;
//     }
// }

// 1. Create the ring
// 2. Check whether non-vectored read is supported using io_uring_probe
// 3. If so, submit read(v) operations
// 4. Reap read-completed completion queue entries
// 5. Tear-down

//static int setup_context(unsigned entries, struct io_uring *ring) { 
//}

#define NUM_FILES 64

struct RIOVec { 
	void *fBuffer; 
	off_t fOffset; 
	size_t fSize; 
	size_t fOutBytes; 
}; 

struct ReadData { 
	const char* pathname; 
	struct RIOVec io_data; 
}; 

static int prep_reads(struct io_uring *ring, ReadData *files, int num_files) { 
	struct io_uring_sqe *sqe; 
	int fd = -1; 
	for (int i = 0; i < num_files; i++) { 
		fd = open(files[i].pathname, O_RDONLY); 
		if (fd < 0) { 
			perror("open"); 
			return 1; 
		}	
		sqe = io_uring_get_sqe(ring); 
		if (!sqe) { 
			fprintf(stderr, "sqe get failed\n"); 
			return 1; 
		}

		io_uring_prep_read(sqe, fd, 0, 0, 0); 
	}
	return 0; 
}

static int reap_reads(struct io_uring *ring, int num_files) { 
	struct io_uring_cqe *cqe;
	int ret; 

	for (int i = 0; i < num_files; i++) { 
		// won't hang, may return failure cqe 
		ret = io_uring_wait_cqe(ring, &cqe); 
		if (ret) { 
			fprintf(stderr, "wait cqe: %d\n", ret); 
		}
	}
	return 0; 
}

int main(int argc, char* argv[]) {
    struct io_uring ring;
	struct io_uring_probe *p; 
	int ret; 

	ret = io_uring_queue_init(NUM_FILES, &ring, 0 /* flags */);
	if (ret) { 
	    fprintf(stderr, "ring create failed: %d\n", ret); 
		return 1; 		
	}
	// kinda dumb, could do readv with length one 
	// -- for simplicity
	p = io_uring_get_probe_ring(&ring); 
	if (!p || !io_uring_opcode_supported(p, IORING_OP_READ)) { 
	    fprintf(stderr, "read syscall not supported, exiting: %d\n", ret); 
		return 1; 
	}
	free(p); 
	
	ReadData files[NUM_FILES]; 
	files[0].pathname = "file.txt"; 
	files[0].io_data.fBuffer = NULL; 
	files[0].io_data.fOffset = 0; 
	files[0].io_data.fSize = 0; 
	files[0].io_data.fOutBytes = 0; 

	// ret = prep_reads(&ring, files, NUM_FILES); 
	ret = prep_reads(&ring, files, 1);
	if (ret) {
	    fprintf(stderr, "prep reads failed: %d\n", ret); 
		return 1; 		
	}
	
	ret = io_uring_submit(&ring); 
	if (ret) {
	    fprintf(stderr, "submit failed: %d\n", ret); 
		return 1; 		
	}
	
	ret = reap_reads(&ring, NUM_FILES); 
	if (ret) {
	    fprintf(stderr, "reap reads failed: %d\n", ret); 
		return 1; 		
	}

    io_uring_queue_exit(&ring);
    return 0;
}
