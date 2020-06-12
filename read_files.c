#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	void *fBuffer; 
	off_t fOffset; 
	size_t fSize; 
	size_t fOutBytes; 
}; 

struct ReadData { 
	const char* pathname; 
	struct RIOVec io_data; 
}; 

static int prep_reads(struct io_uring *ring, ReadData files[], int num_files) { 
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

		io_uring_prep_read(sqe, fd, files[i].io_data.fBuffer, 
		    files[i].io_data.fSize, files[i].io_data.fOffset); 
		
		// mark position in files array 
		sqe->user_data = i; 
	}
	return 0; 
}

static int reap_reads(struct io_uring *ring, ReadData files[], int num_files) { 
	struct io_uring_cqe *cqe;
	int ret; 

	for (int i = 0; i < num_files; i++) { 
		// if right number of sqes are submitted, won't hang 
		// but may return failure cqe 
		// if too few sqes are submitted, could wait forever 
		// -- switch to timeout 
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
		files[index].io_data.fOutBytes = (size_t)cqe->res; 
		printf("read %lu bytes from file %lu\n", files[index].io_data.fOutBytes, index); 

		// advance ring 
		io_uring_cqe_seen(ring, cqe); 
	}
	return 0; 
}

// caller responsible for freeing using free_read_data
#define BUF_SIZE 1024
ReadData make_read_data(const char* pathname) { 
	ReadData rd; 
	rd.pathname = pathname; 
	rd.io_data.fBuffer = malloc(BUF_SIZE); 
	rd.io_data.fOffset = 0; 
	rd.io_data.fSize = BUF_SIZE; 
	rd.io_data.fOutBytes = 0; // set by cqe 
	return rd; 
}

void free_read_data(ReadData *read_data) { 
	if (NULL != read_data->io_data.fBuffer) { 
		free(read_data->io_data.fBuffer); 
	}
}

#define NUM_FILES 1

int main(int argc, char* argv[]) {

	//if (argc < 2) { 
	//	printf("%s: file [files...]\n", argv[0]) ; 
	//	return 1; 
	//}	

    struct io_uring ring;
	struct io_uring_probe *p; 
	int ret; 

	ret = io_uring_queue_init(NUM_FILES, &ring, 0 /* flags */);
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
	
	ReadData files[NUM_FILES]; 
	files[0] = make_read_data("file.txt"); 
	if (files[0].io_data.fBuffer == NULL) { 
		perror("malloc"); 
		return 1; 
	}

	ret = prep_reads(&ring, files, NUM_FILES); 
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
	
	ret = reap_reads(&ring, files, NUM_FILES); 
	if (ret) {
	    fprintf(stderr, "reap reads failed: %d\n", ret); 
		return 1; 		
	}
	
	free_read_data(&files[0]); 
    io_uring_queue_exit(&ring);
    return 0;
}
