#include "my_vm.h"

#define get_pde_bits(x, pde_width) ((x) >> (ADDRESS_BITS - pde_width))
#define get_pte_bits(x, pte_width, offset) ((x >> (offset)) & ((1UL << (pte_width)) - 1))
#define get_offset_bits(x, offset_bits) ((x) & ((1UL << (offset_bits)) - 1))
static Bitmap phys_bitmap;
static Bitmap virt_bitmap;
static pde_t* page_directory;
static TLB tlb;
static unsigned long num_phys_pages;
static unsigned long num_virt_pages;
static char* physical_memory;
static char* disk;
static int is_initialized = 0;
static unsigned long tlb_mismatch_count = 0;
static unsigned long total_allocs = 0;
static unsigned long total_frees = 0;
static pthread_mutex_t vm_mutex = PTHREAD_MUTEX_INITIALIZER;
queue page_queue;
const int offset_bits = log2(PAGE_SIZE);
const int pde_bits = (ADDRESS_BITS - offset_bits) / 2;
const int pte_bits = ADDRESS_BITS - offset_bits - pde_bits;

void initBitmap(Bitmap* bitmap, unsigned long num_pages) {
    bitmap->num_pages = num_pages;
    bitmap->bitmap = (unsigned char*)calloc((num_pages + 7) / 8, sizeof(unsigned char));
    bitmap->free_pages = num_pages;
}
void setBitmap(Bitmap* bitmap, unsigned long page_num) {
    unsigned char mask = 1 << (page_num % 8);
    if (!(bitmap->bitmap[page_num / 8] & mask)) {
        bitmap->bitmap[page_num / 8] |= mask;
        bitmap->free_pages--;
    }
}
void clearBitmap(Bitmap* bitmap, unsigned long page_num) {
    unsigned char mask = 1 << (page_num % 8);
    if (bitmap->bitmap[page_num / 8] & mask) {
        bitmap->bitmap[page_num / 8] &= ~mask;
        bitmap->free_pages++;
    }
}
bool isBitmapSet(Bitmap* bitmap, unsigned long page_num) {
    return (bitmap->bitmap[page_num / 8] & (1 << (page_num % 8))) != 0;
}

void queue_init(queue* q) {
    q->head = NULL;
    q->tail = NULL;
}
void queue_push(queue* q, unsigned long data) {
    node* new_node = (node*)malloc(sizeof(node));
    new_node->data = data;
    new_node->next = NULL;
    if (q->tail) {
        q->tail->next = new_node;
    }
    else {
        q->head = new_node;
    }
    q->tail = new_node;
}
unsigned long queue_pop(queue* q) {
    if (q->head) {
        node* temp = q->head;
        unsigned long data = temp->data;
        q->head = q->head->next;
        free(temp);
        if (!q->head) {
            q->tail = NULL;
        }
        return data;
    }
    return 0;
}

void queue_remove(queue* q, unsigned long data) {
    node* prev = NULL;
    node* cur = q->head;
    while (cur) {
        if (cur->data == data) {
            if (prev) prev->next = cur->next;
            else q->head = cur->next;
            if (cur == q->tail) q->tail = prev;
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/*
Function responsible for allocating and setting your simulated physical memory and disk space
*/
void initMemoryAndDisk() {
    if (is_initialized) return;
    num_phys_pages = PM_SIZE / PAGE_SIZE;
    num_virt_pages = VM_SIZE / PAGE_SIZE;
    initBitmap(&phys_bitmap, num_phys_pages);
    initBitmap(&virt_bitmap, num_virt_pages);
    page_directory = (pde_t*)calloc((1UL << pde_bits), sizeof(pde_t));
    memset(&tlb, 0, sizeof(TLB));
    physical_memory = (char*)malloc(PM_SIZE);
    disk = (char*)malloc(DISK_SIZE);
    queue_init(&page_queue);
    is_initialized = 1;
}

/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
pte_t* translate(pde_t* pgdir, void* va) {
    unsigned long va_num = sanitizeVA(va);
    unsigned long pde_index = get_pde_bits(va_num, pde_bits);
    unsigned long pte_index = get_pte_bits(va_num, pte_bits, offset_bits);
    unsigned long offset = get_offset_bits(va_num, offset_bits);

    if (pde_index >= (1UL << pde_bits)) return NULL;
    if (!pgdir[pde_index] && pageFault(pgdir, va) == -1) return NULL;

    pte_t* page_table = (pte_t*)pgdir[pde_index];
    if (pte_index >= (1UL << pte_bits)) return NULL;
    if (!page_table[pte_index] && pageFault(pgdir, va) == -1) return NULL;

    pte_t pte = page_table[pte_index];
    pte_t* pt_phys = (pte_t*)(physical_memory + pte + offset);

    // Always use page table as source of truth; TLB as cache
    pte_t* tlb_phys = checkTLB(va);
    if (tlb_phys) {
        tlb.tlb_accesses++;
        // Cross-validate TLB against page table
        if (tlb_phys != pt_phys) {
            tlb_mismatch_count++;
            invalidateTLB(va);
            addTLB(va, (void*)pte);
        }
    } else {
        tlb.tlb_misses++;
        addTLB(va, (void*)pte);
    }

    return pt_phys;
}

/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int pageMap(pde_t* pgdir, void* va, void* pa) {
    unsigned long va_num = sanitizeVA(va);
    unsigned long pde_index = get_pde_bits(va_num, pde_bits);
    unsigned long pte_index = get_pte_bits(va_num, pte_bits, offset_bits);
    if (pde_index >= (1UL << pde_bits)) return -1;
    if (pte_index >= (1UL << pte_bits)) return -1;
    if (!pgdir[pde_index]) {
        pte_t* new_page_table = (pte_t*)calloc((1UL << pte_bits), sizeof(pte_t));
        pgdir[pde_index] = (unsigned long)new_page_table;
    }
    pte_t* page_table = (pte_t*)pgdir[pde_index];
    page_table[pte_index] = (unsigned long)pa;
    return 0;
}

/* Function responsible for allocating pages
and used by the benchmark
*/
void* myMalloc(unsigned int num_bytes) {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    unsigned int num_pages = (num_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages == 0) { pthread_mutex_unlock(&vm_mutex); return NULL; }
    unsigned long start_vpage = 0;
    unsigned int found = 0;
    // Reserve virtual page 0 to avoid returning NULL-equivalent pointer (0)
    for (unsigned long i = 1; i < num_virt_pages; i++) {
        if (isBitmapSet(&virt_bitmap, i)) found = 0;
        else {
            if (!found) start_vpage = i;
            found++;
            if (found == num_pages) break;
        }
    }
    if (found < num_pages) {
        pthread_mutex_unlock(&vm_mutex); return NULL;
    }
    unsigned long cur_page = 0;
    for (unsigned int i = start_vpage; i < start_vpage + num_pages; i++) {
        for (; cur_page < num_phys_pages; cur_page++) {
            if (!isBitmapSet(&phys_bitmap, cur_page)) break;
        }
        if (cur_page >= num_phys_pages) {
            for (unsigned int k = start_vpage; k < i; k++)
                clearBitmap(&virt_bitmap, k);
            pthread_mutex_unlock(&vm_mutex); return NULL;
        }
        setBitmap(&virt_bitmap, i);
        setBitmap(&phys_bitmap, cur_page);
        void* va = (void*)(i << offset_bits);
        void* pa = (void*)(cur_page << offset_bits);
        pageMap(page_directory, va, pa);
        addTLB(va, pa);
        queue_push(&page_queue, i);
        cur_page++;
    }
    void* result = (void*)(start_vpage << offset_bits);
    total_allocs += num_pages;
    pthread_mutex_unlock(&vm_mutex);
    return result;
}

/* Responsible for releasing one or more memory pages using virtual address (va)
*/
void myFree(void* va, int size) {
    if (!va) return;
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    unsigned long va_num = sanitizeVA(va);
    unsigned long start_vpage = va_num >> offset_bits;
    unsigned int num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (unsigned int i = 0; i < num_pages; i++) {
        unsigned long vpage = start_vpage + i;
        if (!isBitmapSet(&virt_bitmap, vpage)) {
            printf("Segmentation Fault\n");
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        invalidateTLB((void*)(vpage << offset_bits));
        clearBitmap(&virt_bitmap, vpage);
        queue_remove(&page_queue, vpage);
        unsigned long pde_index = get_pde_bits(vpage << offset_bits, pde_bits);
        unsigned long pte_index = get_pte_bits(vpage << offset_bits, pte_bits, offset_bits);
        pte_t* page_table = (pte_t*)page_directory[pde_index];
        unsigned long ppn = page_table[pte_index] >> offset_bits;
        clearBitmap(&phys_bitmap, ppn);
        page_table[pte_index] = 0;
        int count = 1 << (pte_bits);
        bool ept = true;
        for (int j = 0; j < count; j++) {
            if (page_table[j]) {
                ept = false;
                break;
            }
        }
        if (ept) {
            free(page_table);
            page_directory[pde_index] = 0;
        }
    }
    total_frees += num_pages;
    pthread_mutex_unlock(&vm_mutex);
}

/* The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
*/
void myWrite(void* va, void* val, int size) {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    while (size) {
        pte_t* phys = translate(page_directory, va);
        if (!phys) {
            printf("ERROR: Writing to unallocated address\n");
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        if ((char*)phys < physical_memory || (char*)phys >= physical_memory + PM_SIZE) {
            printf("FATAL: OOB phys=%p for va=%p\n", phys, va);
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        unsigned long offset = (unsigned long)va & (PAGE_SIZE - 1);
        unsigned long to_write = PAGE_SIZE - offset < size ? PAGE_SIZE - offset : size;
        memcpy(phys, val, to_write);
        // Verify write took effect
        if (memcmp(phys, val, to_write) != 0) {
            printf("VERIFY FAIL: va=%p wrote=%x readback=%x\n", va, *(int*)val, *(int*)phys);
        }
        size -= to_write;
        val = (char*)val + to_write;
        va = (char*)va + to_write;
    }
    pthread_mutex_unlock(&vm_mutex);
}

/*Given a virtual address, this function copies the contents of the page to val*/
void myRead(void* va, void* val, int size) {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    while (size) {
        pte_t* phys = translate(page_directory, va);
        if (!phys) {
            printf("ERROR: Reading from unallocated address\n");
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        unsigned long offset = (unsigned long)va & (PAGE_SIZE - 1);
        unsigned long to_read = PAGE_SIZE - offset < size ? PAGE_SIZE - offset : size;
        memcpy(val, phys, to_read);
        size -= to_read;
        val = (char*)val + to_read;
        va = (char*)va + to_read;
    }
    pthread_mutex_unlock(&vm_mutex);
}

int pageFault(pde_t* pgdir, void* va) {
    unsigned long va_num = sanitizeVA(va);
    unsigned long vpn = va_num >> offset_bits;
    unsigned long cur_page = 0;
    if (phys_bitmap.free_pages == 0) {
        if (!page_queue.head) return -1;
        unsigned long evict_vpn = queue_pop(&page_queue);
        unsigned long evict_va = evict_vpn << offset_bits;
        unsigned long evict_pde = get_pde_bits(evict_va, pde_bits);
        unsigned long evict_pte = get_pte_bits(evict_va, pte_bits, offset_bits);
        pte_t* pt = (pte_t*)pgdir[evict_pde];
        unsigned long evict_ppn = pt[evict_pte] >> offset_bits;
        memcpy(disk + evict_vpn * PAGE_SIZE, physical_memory + evict_ppn * PAGE_SIZE, PAGE_SIZE);
        pt[evict_pte] = 0;
        clearBitmap(&phys_bitmap, evict_ppn);
        clearBitmap(&virt_bitmap, evict_vpn);
        invalidateTLB((void*)evict_va);
        cur_page = evict_ppn;
    }
    for (; cur_page < num_phys_pages; cur_page++) {
        if (!isBitmapSet(&phys_bitmap, cur_page)) break;
    }
    if (cur_page >= num_phys_pages) return -1;
    void* pa = (void*)(cur_page << offset_bits);
    if (pageMap(pgdir, va, pa) == -1) return -1;
    if (!isBitmapSet(&phys_bitmap, cur_page)) {
        setBitmap(&phys_bitmap, cur_page);
    }
    if (!isBitmapSet(&virt_bitmap, vpn)) {
        setBitmap(&virt_bitmap, vpn);
        queue_push(&page_queue, vpn);
    }
    return 0;
}

pte_t* checkTLB(void* va) {
    unsigned long va_num = sanitizeVA(va);
    unsigned long offset = va_num & (PAGE_SIZE - 1);
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb.entry[i].valid && tlb.entry[i].v_page == (va_num >> offset_bits)) {
            return (pte_t*)(physical_memory + tlb.entry[i].p_page + offset);
        }
    }
    return NULL;
}

void findnext() {
    bool isvalid = true;
    int maxi = 0, maxetime = -1;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (!tlb.entry[i].valid) {
            if (!isvalid) continue;
            isvalid = false;
            maxi = i;
        }
        else {
            tlb.entry[i].etime++;
            if (!isvalid) continue;
            if (tlb.entry[i].etime > maxetime) {
                maxetime = tlb.entry[i].etime;
                maxi = i;
            }
        }
    }
    tlb.next_replace = maxi;
}

int addTLB(void* va, void* pa) {
    unsigned long vpn = sanitizeVA(va) >> offset_bits;
    unsigned long ppn = (unsigned long)pa >> offset_bits;
    tlb.entry[tlb.next_replace].valid = true;
    tlb.entry[tlb.next_replace].v_page = vpn;
    tlb.entry[tlb.next_replace].p_page = ppn << offset_bits;
    tlb.entry[tlb.next_replace].etime = 0;
    findnext();
    return 0;
}

void invalidateTLB(void* va) {
    unsigned long vpn = sanitizeVA(va) >> offset_bits;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb.entry[i].valid && tlb.entry[i].v_page == vpn) {
            tlb.entry[i].valid = false;
        }
    }
    findnext();
}

void cleanupMemoryAndDisk() {
    pthread_mutex_lock(&vm_mutex);
    if (!is_initialized) {
        pthread_mutex_unlock(&vm_mutex);
        return;
    }
    for (unsigned int i = 0; i < (1UL << pde_bits); i++) {
        if (page_directory[i])
            free((pte_t*)page_directory[i]);
    }
    free(page_directory);
    free(phys_bitmap.bitmap);
    free(virt_bitmap.bitmap);
    free(physical_memory);
    free(disk);
    while (page_queue.head) {
        node* tmp = page_queue.head;
        page_queue.head = page_queue.head->next;
        free(tmp);
    }
    is_initialized = 0;
    pthread_mutex_unlock(&vm_mutex);
}

void printTLBStats() {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    unsigned int total = tlb.tlb_accesses + tlb.tlb_misses;
    printf("TLB Total Accesses: %u\n", total);
    if (total > 0) {
        printf("TLB Hit Rate: %.2f%%\n", (double)tlb.tlb_accesses / total * 100);
    }
    pthread_mutex_unlock(&vm_mutex);
}