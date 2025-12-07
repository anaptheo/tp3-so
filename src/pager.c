#include "pager.h"
#include "mmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>

#include <sys/mman.h>
#include <unistd.h>

typedef struct Page {
    intptr_t vaddr;
    int frame;      // -1 if not in memory
    int block;      // -1 if not on disk
    int resident;   // 1 if in memory, 0 otherwise
    int referenced; // for second chance
    int dirty;      // 1 if written to
    int initialized; // 1 if mmu_zero_fill called or loaded from disk
    int disk_valid; // 1 if disk block contains valid data
    int prot;       // Current protection level
    struct Page *next;
} Page;

typedef struct Process {
    pid_t pid;
    Page *pages;
    struct Process *next;
} Process;

typedef struct Frame {
    int in_use;
    pid_t pid;
    intptr_t vaddr; // to easily find the page struct if needed, or just use pid/vaddr to search
} Frame;

// Global variables
int g_nframes;
int g_nblocks;
Frame *g_frames;
int *g_blocks; // 0 = free, 1 = used
Process *g_processes = NULL;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// Second chance algorithm state
int g_clock_hand = 0;

void pager_init(int nframes, int nblocks) {
    pthread_mutex_lock(&g_mutex);
    g_nframes = nframes;
    g_nblocks = nblocks;

    g_frames = malloc(sizeof(Frame) * nframes);
    for (int i = 0; i < nframes; i++) {
        g_frames[i].in_use = 0;
        g_frames[i].pid = -1;
        g_frames[i].vaddr = 0;
    }

    g_blocks = malloc(sizeof(int) * nblocks);
    for (int i = 0; i < nblocks; i++) {
        g_blocks[i] = 0;
    }
    pthread_mutex_unlock(&g_mutex);
}

void pager_create(pid_t pid) {
    pthread_mutex_lock(&g_mutex);
    Process *proc = malloc(sizeof(Process));
    proc->pid = pid;
    proc->pages = NULL;
    proc->next = g_processes;
    g_processes = proc;
    pthread_mutex_unlock(&g_mutex);
}

void pager_destroy(pid_t pid) {
    pthread_mutex_lock(&g_mutex);
    Process **curr = &g_processes;
    while (*curr) {
        if ((*curr)->pid == pid) {
            Process *to_free = *curr;
            *curr = (*curr)->next;
            
            // Free pages and resources
            Page *p = to_free->pages;
            while (p) {
                Page *next_p = p->next;
                if (p->frame != -1) {
                    g_frames[p->frame].in_use = 0;
                    g_frames[p->frame].pid = -1;
                }
                if (p->block != -1) {
                    g_blocks[p->block] = 0;
                }
                free(p);
                p = next_p;
            }
            free(to_free);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

void *pager_extend(pid_t pid) {
    pthread_mutex_lock(&g_mutex);
    
    // Find process
    Process *proc = g_processes;
    while (proc && proc->pid != pid) {
        proc = proc->next;
    }
    
    if (!proc) {
        pthread_mutex_unlock(&g_mutex);
        return NULL; // Should not happen if pager_create was called
    }

    // Allocate disk block
    int block = -1;
    for (int i = 0; i < g_nblocks; i++) {
        if (g_blocks[i] == 0) {
            g_blocks[i] = 1;
            block = i;
            break;
        }
    }

    if (block == -1) {
        pthread_mutex_unlock(&g_mutex);
        return NULL;
    }

    // Create new page
    Page *page = malloc(sizeof(Page));
    
    // Calculate vaddr
    // If first page, UVM_BASEADDR. Else, last page + PAGESIZE (assumed 4096 based on mmu.h comments or sysconf)
    // mmu.h says UVM_BASEADDR is 0x60000000.
    // We need to find the last page to determine the new address.
    
    intptr_t new_vaddr = UVM_BASEADDR;
    Page *last = proc->pages;
    if (last) {
        while (last->next) {
            last = last->next;
        }
        new_vaddr = last->vaddr + sysconf(_SC_PAGESIZE);
    } else {
        // First page
        new_vaddr = UVM_BASEADDR;
    }
    
    // Check bounds? UVM_MAXADDR is 0x600FFFFF.
    if (new_vaddr > UVM_MAXADDR) {
        g_blocks[block] = 0; // Release block
        free(page);
        pthread_mutex_unlock(&g_mutex);
        return NULL;
    }

    page->vaddr = new_vaddr;
    page->frame = -1;
    page->block = block;
    page->resident = 0;
    page->referenced = 0;
    page->dirty = 0;
    page->initialized = 0;
    page->disk_valid = 0;
    page->next = NULL;

    if (proc->pages) {
        last->next = page;
    } else {
        proc->pages = page;
    }

    pthread_mutex_unlock(&g_mutex);
    return (void *)new_vaddr;
}

void _pager_fault(pid_t pid, void *addr) {
    intptr_t vaddr = (intptr_t)addr;
    // Align to page boundary
    vaddr = vaddr & ~(sysconf(_SC_PAGESIZE) - 1);

    // Find process
    Process *proc = g_processes;
    while (proc && proc->pid != pid) {
        proc = proc->next;
    }
    
    if (!proc) return;

    // Find page
    Page *page = proc->pages;
    while (page && page->vaddr != vaddr) {
        page = page->next;
    }

    if (!page) return;

    if (page->resident) {
        // Resident page fault -> Protection fault
        if (page->prot == PROT_NONE) {
            // Was waiting for second chance access
            page->referenced = 1;
            page->prot = PROT_READ;
            mmu_chprot(pid, (void*)vaddr, PROT_READ);
        } else if (page->prot == PROT_READ) {
            // Write access (assumed, since it faulted on READ)
            page->referenced = 1;
            page->dirty = 1;
            page->prot = PROT_READ | PROT_WRITE;
            mmu_chprot(pid, (void*)vaddr, PROT_READ | PROT_WRITE);
        }
    } else {
        // Non-resident page fault -> Bring into memory
        int frame = -1;
        
        // Look for free frame
        for (int i = 0; i < g_nframes; i++) {
            if (!g_frames[i].in_use) {
                frame = i;
                break;
            }
        }

        // If no free frame, evict
        if (frame == -1) {
            while (frame == -1) {
                Frame *f = &g_frames[g_clock_hand];
                
                // Find owner page
                Process *owner = g_processes;
                while (owner && owner->pid != f->pid) {
                    owner = owner->next;
                }
                Page *owner_page = NULL;
                if (owner) {
                    owner_page = owner->pages;
                    while (owner_page && owner_page->vaddr != f->vaddr) {
                        owner_page = owner_page->next;
                    }
                }

                if (owner_page) {
                    if (owner_page->referenced) {
                        owner_page->referenced = 0;
                        owner_page->prot = PROT_NONE;
                        mmu_chprot(f->pid, (void*)f->vaddr, PROT_NONE);
                    } else {
                        // Evict
                        mmu_nonresident(f->pid, (void*)f->vaddr);
                        
                        if (owner_page->dirty) {
                            mmu_disk_write(g_clock_hand, owner_page->block);
                            owner_page->dirty = 0;
                            owner_page->disk_valid = 1;
                        }
                        owner_page->resident = 0;
                        owner_page->frame = -1;
                        owner_page->prot = 0;
                        
                        f->in_use = 0;
                        frame = g_clock_hand;
                    }
                } else {
                    f->in_use = 0;
                    frame = g_clock_hand;
                }

                g_clock_hand = (g_clock_hand + 1) % g_nframes;
            }
        }

        // Use frame
        g_frames[frame].in_use = 1;
        g_frames[frame].pid = pid;
        g_frames[frame].vaddr = vaddr;

        if (page->disk_valid) {
            mmu_disk_read(page->block, frame);
        } else {
            mmu_zero_fill(frame);
        }
        page->initialized = 1;

        page->frame = frame;
        page->resident = 1;
        page->referenced = 1;
        page->prot = PROT_READ;
        page->dirty = 0;
        
        mmu_resident(pid, (void*)vaddr, frame, PROT_READ);
    }
}

void pager_fault(pid_t pid, void *addr) {
    pthread_mutex_lock(&g_mutex);
    _pager_fault(pid, addr);
    pthread_mutex_unlock(&g_mutex);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    pthread_mutex_lock(&g_mutex);
    
    Process *proc = g_processes;
    while (proc && proc->pid != pid) {
        proc = proc->next;
    }
    
    if (!proc) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    // Validate range
    for (size_t i = 0; i < len; i++) {
        intptr_t curr_vaddr = (intptr_t)addr + i;
        intptr_t page_vaddr = curr_vaddr & ~(sysconf(_SC_PAGESIZE) - 1);
        
        Page *page = proc->pages;
        while (page && page->vaddr != page_vaddr) {
            page = page->next;
        }
        
        if (!page) {
            pthread_mutex_unlock(&g_mutex);
            return -1;
        }
    }

    // Print
    for (size_t i = 0; i < len; i++) {
        intptr_t curr_vaddr = (intptr_t)addr + i;
        intptr_t page_vaddr = curr_vaddr & ~(sysconf(_SC_PAGESIZE) - 1);
        int offset = curr_vaddr & (sysconf(_SC_PAGESIZE) - 1);

        Page *page = proc->pages;
        while (page && page->vaddr != page_vaddr) {
            page = page->next;
        }

        // Ensure resident (read access)
        if (!page->resident || page->prot == PROT_NONE) {
            _pager_fault(pid, (void*)curr_vaddr);
        }
        // If it was PROT_READ, it's fine. If PROT_WRITE, also fine.
        // But _pager_fault might have just made it resident.
        
        // Access pmem
        // pmem is char*, so we can index it.
        // frame index * pagesize + offset
        int frame = page->frame;
        printf("%02x", (unsigned char)pmem[frame * sysconf(_SC_PAGESIZE) + offset]);
    }
    if (len > 0) {
        printf("\n");
    }
    
    pthread_mutex_unlock(&g_mutex);
    return 0;
}
