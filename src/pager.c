#include "pager.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mmu.h"

/* Page metadata tracked by the pager. */
struct page_info {
	int allocated;
	void *vaddr;
	int block;
	int frame;
	int prot;
	int dirty;
	int on_disk;
};

/* Per-process bookkeeping. */
struct process_info {
	pid_t pid;
	size_t npages;
	size_t capacity;
	int active;
	struct page_info *pages;
};

/* Physical frame metadata used by the clock algorithm. */
struct frame_info {
	int used;
	pid_t pid;
	size_t page_idx;
	int referenced;
};

static pthread_mutex_t pager_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct frame_info *frames = NULL;
static int *block_used = NULL;
static struct process_info *procs = NULL;
static size_t procs_count = 0;
static size_t procs_capacity = 0;
static int total_frames = 0;
static int total_blocks = 0;
static size_t page_size = 0;
static size_t max_pages = 0;
static int clock_hand = 0;

static struct process_info *find_process(pid_t pid);
static struct process_info *alloc_process(pid_t pid);
static int allocate_block(void);
static void release_process_resources(struct process_info *p);
static size_t addr_to_index(void *addr);
static int find_free_frame(void);
static int evict_frame(void);
static int obtain_frame(void);
static void mark_frame(int frame, pid_t pid, size_t page_idx);
static void map_page_into_frame(struct process_info *p, size_t page_idx, int frame);
static void handle_resident_access(struct process_info *p, struct page_info *pg, int frame, int write_request);

void pager_init(int nframes, int nblocks) {
	pthread_mutex_lock(&pager_mutex);
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	max_pages = (size_t)((UVM_MAXADDR - UVM_BASEADDR + 1) / page_size);
	total_frames = nframes;
	total_blocks = nblocks;
	clock_hand = 0;

	frames = calloc((size_t)nframes, sizeof(*frames));
	block_used = calloc((size_t)nblocks, sizeof(*block_used));
	procs_capacity = 4;
	procs = calloc(procs_capacity, sizeof(*procs));
	pthread_mutex_unlock(&pager_mutex);
}

void pager_create(pid_t pid) {
	pthread_mutex_lock(&pager_mutex);
	alloc_process(pid);
	pthread_mutex_unlock(&pager_mutex);
}

void *pager_extend(pid_t pid) {
	pthread_mutex_lock(&pager_mutex);
	struct process_info *p = find_process(pid);
	if(!p) {
		pthread_mutex_unlock(&pager_mutex);
		return NULL;
	}
	if(p->npages >= max_pages) {
		errno = ENOSPC;
		pthread_mutex_unlock(&pager_mutex);
		return NULL;
	}
	int block = allocate_block();
	if(block == -1) {
		errno = ENOSPC;
		pthread_mutex_unlock(&pager_mutex);
		return NULL;
	}

	size_t idx = p->npages;
	struct page_info *pg = &p->pages[idx];
	pg->allocated = 1;
	pg->vaddr = (void *)(UVM_BASEADDR + idx * page_size);
	pg->block = block;
	pg->frame = -1;
	pg->prot = PROT_NONE;
	pg->dirty = 0;
	pg->on_disk = 0;
	p->npages++;

	void *ret = pg->vaddr;
	pthread_mutex_unlock(&pager_mutex);
	return ret;
}

void pager_fault(pid_t pid, void *addr) {
	pthread_mutex_lock(&pager_mutex);
	struct process_info *p = find_process(pid);
	if(!p) {
		pthread_mutex_unlock(&pager_mutex);
		return;
	}
	size_t idx = addr_to_index(addr);
	if(idx >= p->npages) {
		pthread_mutex_unlock(&pager_mutex);
		return;
	}
	struct page_info *pg = &p->pages[idx];

	if(pg->frame != -1) {
		int write_request = (pg->prot == PROT_READ);
		handle_resident_access(p, pg, pg->frame, write_request);
		pthread_mutex_unlock(&pager_mutex);
		return;
	}

	int frame = obtain_frame();
	map_page_into_frame(p, idx, frame);
	pthread_mutex_unlock(&pager_mutex);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
	pthread_mutex_lock(&pager_mutex);
	struct process_info *p = find_process(pid);
	if(!p) {
		errno = EINVAL;
		pthread_mutex_unlock(&pager_mutex);
		return -1;
	}
	uintptr_t start = (uintptr_t)addr;
	uintptr_t base = (uintptr_t)UVM_BASEADDR;
	if(start < base) {
		errno = EINVAL;
		pthread_mutex_unlock(&pager_mutex);
		return -1;
	}
	size_t offset = (size_t)(start - base);
	size_t allocated = p->npages * page_size;
	if(offset > allocated || len > allocated - offset) {
		errno = EINVAL;
		pthread_mutex_unlock(&pager_mutex);
		return -1;
	}
	if(len == 0) {
		pthread_mutex_unlock(&pager_mutex);
		return 0;
	}

	unsigned char *buf = malloc(len);
	if(!buf) {
		pthread_mutex_unlock(&pager_mutex);
		return -1;
	}

	size_t done = 0;
	while(done < len) {
		size_t cur = offset + done;
		size_t page_idx = cur / page_size;
		size_t inpage = cur % page_size;
		size_t chunk = len - done;
		if(chunk > page_size - inpage) {
			chunk = page_size - inpage;
		}
		struct page_info *pg = &p->pages[page_idx];
		if(pg->frame == -1) {
			int frame = obtain_frame();
			map_page_into_frame(p, page_idx, frame);
		} else {
			handle_resident_access(p, pg, pg->frame, 0);
		}
		if(pg->frame != -1) {
			frames[pg->frame].referenced = 1;
		}
		size_t phys_off = (size_t)pg->frame * page_size + inpage;
		memcpy(buf + done, pmem + phys_off, chunk);
		done += chunk;
	}
	pthread_mutex_unlock(&pager_mutex);

	for(size_t i = 0; i < len; ++i) {
		printf("%02x", (unsigned)buf[i]);
	}
	printf("\n");
	free(buf);
	return 0;
}

void pager_destroy(pid_t pid) {
	pthread_mutex_lock(&pager_mutex);
	struct process_info *p = find_process(pid);
	if(!p) {
		pthread_mutex_unlock(&pager_mutex);
		return;
	}
	release_process_resources(p);
	pthread_mutex_unlock(&pager_mutex);
}

static struct process_info *find_process(pid_t pid) {
	for(size_t i = 0; i < procs_count; ++i) {
		if(procs[i].active && procs[i].pid == pid) {
			return &procs[i];
		}
	}
	return NULL;
}

static struct process_info *alloc_process(pid_t pid) {
	struct process_info *slot = NULL;
	for(size_t i = 0; i < procs_count; ++i) {
		if(!procs[i].active) {
			slot = &procs[i];
			break;
		}
	}
	if(!slot) {
		if(procs_count == procs_capacity) {
			size_t newcap = procs_capacity ? procs_capacity * 2 : 4;
			struct process_info *tmp = realloc(procs, newcap * sizeof(*procs));
			if(!tmp) exit(EXIT_FAILURE);
			procs = tmp;
			for(size_t i = procs_capacity; i < newcap; ++i) {
				memset(&procs[i], 0, sizeof(procs[i]));
			}
			procs_capacity = newcap;
		}
		slot = &procs[procs_count++];
	}
	slot->pid = pid;
	slot->npages = 0;
	slot->capacity = max_pages;
	slot->active = 1;
	slot->pages = calloc(max_pages, sizeof(*slot->pages));
	return slot;
}

static int allocate_block(void) {
	for(int i = 0; i < total_blocks; ++i) {
		if(!block_used[i]) {
			block_used[i] = 1;
			return i;
		}
	}
	return -1;
}

static void release_process_resources(struct process_info *p) {
	for(size_t i = 0; i < p->npages; ++i) {
		struct page_info *pg = &p->pages[i];
		if(!pg->allocated) continue;
		if(pg->frame != -1) {
			int frame = pg->frame;
			frames[frame].used = 0;
			frames[frame].referenced = 0;
			frames[frame].pid = 0;
			frames[frame].page_idx = 0;
			pg->frame = -1;
		}
		if(pg->block >= 0 && pg->block < total_blocks) {
			block_used[pg->block] = 0;
		}
	}
	free(p->pages);
	p->pages = NULL;
	p->npages = 0;
	p->active = 0;
}

static size_t addr_to_index(void *addr) {
	return ((uintptr_t)addr - (uintptr_t)UVM_BASEADDR) / page_size;
}

static int find_free_frame(void) {
	for(int i = 0; i < total_frames; ++i) {
		if(!frames[i].used) {
			return i;
		}
	}
	return -1;
}

static int evict_frame(void) {
	while(1) {
		int frame = clock_hand;
		if(!frames[frame].used) {
			clock_hand = (clock_hand + 1) % total_frames;
			return frame;
		}
		struct process_info *p = find_process(frames[frame].pid);
		if(!p) {
			frames[frame].used = 0;
			clock_hand = (clock_hand + 1) % total_frames;
			return frame;
		}
		struct page_info *pg = &p->pages[frames[frame].page_idx];
		if(frames[frame].referenced) {
			mmu_chprot(p->pid, pg->vaddr, PROT_NONE);
			pg->prot = PROT_NONE;
			frames[frame].referenced = 0;
			clock_hand = (clock_hand + 1) % total_frames;
			continue;
		}
		mmu_nonresident(p->pid, pg->vaddr);
		if(pg->dirty) {
			mmu_disk_write(frame, pg->block);
			pg->dirty = 0;
			pg->on_disk = 1;
		}
		pg->frame = -1;
		pg->prot = PROT_NONE;
		frames[frame].used = 0;
		frames[frame].referenced = 0;
		frames[frame].pid = 0;
		frames[frame].page_idx = 0;
		clock_hand = (clock_hand + 1) % total_frames;
		return frame;
	}
}

static int obtain_frame(void) {
	int frame = find_free_frame();
	if(frame != -1) {
		return frame;
	}
	return evict_frame();
}

static void mark_frame(int frame, pid_t pid, size_t page_idx) {
	frames[frame].used = 1;
	frames[frame].pid = pid;
	frames[frame].page_idx = page_idx;
	frames[frame].referenced = 1;
	clock_hand = (frame + 1) % total_frames;
}

static void map_page_into_frame(struct process_info *p, size_t page_idx, int frame) {
	struct page_info *pg = &p->pages[page_idx];
	if(pg->on_disk) {
		mmu_disk_read(pg->block, frame);
	} else {
		mmu_zero_fill(frame);
	}
	mmu_resident(p->pid, pg->vaddr, frame, PROT_READ);
	pg->frame = frame;
	pg->prot = PROT_READ;
	pg->dirty = 0;
	mark_frame(frame, p->pid, page_idx);
}

static void handle_resident_access(struct process_info *p, struct page_info *pg, int frame, int write_request) {
	if(pg->prot == PROT_NONE) {
		int prot = pg->dirty ? (PROT_READ | PROT_WRITE) : PROT_READ;
		mmu_chprot(p->pid, pg->vaddr, prot);
		pg->prot = prot;
	}
	if(write_request && pg->prot == PROT_READ) {
		mmu_chprot(p->pid, pg->vaddr, PROT_READ | PROT_WRITE);
		pg->prot = PROT_READ | PROT_WRITE;
		pg->dirty = 1;
	} else if(write_request && pg->prot == (PROT_READ | PROT_WRITE)) {
		pg->dirty = 1;
	}
	frames[frame].referenced = 1;
}
