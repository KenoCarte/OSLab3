#include "my_vm.h"
#include <string.h>

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
queue page_queue;
const int offset_bits = log2(PAGE_SIZE);
const int pde_bits = (ADDRESS_BITS - offset_bits) / 2;
const int pte_bits = ADDRESS_BITS - offset_bits - pde_bits;
/*
Function responsible for allocating and setting your simulated physical memory and disk space
*/
void initMemoryAndDisk() {

    //Allocate physical memory and disk space using mmap or malloc; this is the total size of
    //your memory/disk you are simulating
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
    //HINT: Also calculate the number of physical and virtual pages and allocate
    //virtual and physical bitmaps and initialize them

}



/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
pte_t* translate(pde_t* pgdir, void* va) {
    if (!is_initialized) initMemoryAndDisk();
    unsigned long va_num = (unsigned long)va;
    unsigned long pde_index = get_pde_bits(va_num, pde_bits);
    unsigned long pte_index = get_pte_bits(va_num, pte_bits, offset_bits);
    unsigned long offset = get_offset_bits(va_num, offset_bits);
    pte_t* tlb_entry = checkTLB(va);
    if (tlb_entry) {
        tlb.tlb_accesses++;
        return tlb_entry;
    }
    tlb.tlb_misses++;
    if (pde_index >= (1UL << pde_bits)) return NULL;
    if (!pgdir[pde_index] && pageFault(pgdir, va) == -1) return NULL;
    pte_t* page_table = (pte_t*)pgdir[pde_index];
    if (pte_index >= (1UL << pte_bits)) return NULL;
    if (!page_table[pte_index] && pageFault(pgdir, va) == -1) return NULL;
    pte_t pte = page_table[pte_index];
    addTLB(va, (void*)pte);
    return (pte_t*)(physical_memory + pte + offset);
}


/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int pageMap(pde_t* pgdir, void* va, void* pa) {
    if (!is_initialized) initMemoryAndDisk();
    unsigned long va_num = (unsigned long)va;
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
    if (!is_initialized) initMemoryAndDisk();
    unsigned int num_pages = (num_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages == 0) return NULL;
    unsigned long start_vpage = 0;
    unsigned int found = 0;
    for (unsigned long i = 0; i < num_virt_pages; i++) {
        if (isBitmapSet(&virt_bitmap, i)) found = 0;
        else {
            if (!found) start_vpage = i;
            found++;
            if (found == num_pages) break;
        }
    }
    if (found < num_pages || phys_bitmap.free_pages < num_pages) return NULL;
    unsigned long cur_page = 0;
    for (unsigned int i = start_vpage;i < start_vpage + num_pages;i++) {
        for (;cur_page < num_phys_pages;cur_page++) {
            if (!isBitmapSet(&phys_bitmap, cur_page)) break;
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
    return (void*)(start_vpage << offset_bits);
}

/* Responsible for releasing one or more memory pages using virtual address (va)
*/
void myFree(void* va, int size) {

    //Free the page table entries starting from this virtual address (va)
    // Also mark the pages free in the bitmap
    //Only free if the memory from "va" to va+size is valid
}


/* The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
*/
void myWrite(void* va, void* val, int size) {

    /* HINT: Using the virtual address and translate(), find the physical page. Copy
       the contents of "val" to a physical page. NOTE: The "size" value can be larger
       than one page. Therefore, you may have to find multiple pages using translate()
       function.*/

}


/*Given a virtual address, this function copies the contents of the page to val*/
void myRead(void* va, void* val, int size) {

    /* HINT: put the values pointed to by "va" inside the physical memory at given
    "val" address. Assume you can access "val" directly by derefencing them.
    If you are implementing TLB,  always check first the presence of translation
    in TLB before proceeding forward */


}

int pageFault(pde_t* pgdir, void* va) {
    unsigned long va_num = (unsigned long)va;
    unsigned long vpn = va_num >> offset_bits;
    if (phys_bitmap.free_pages == 0) {
        if (!page_queue.head) return -1;
        unsigned long evict_vpn = queue_pop(&page_queue);
        unsigned long evict_va = evict_vpn << offset_bits;
        unsigned long evict_pde = get_pde_bits(evict_va, pde_bits);
        unsigned long evict_pte = get_pte_bits(evict_va, pte_bits, offset_bits);
        pte_t* pt = (pte_t*)pgdir[evict_pde];
        unsigned long evict_ppn = pt[evict_pte] >> offset_bits;
        memcpy(disk + evict_vpn * PAGE_SIZE,
               physical_memory + evict_ppn * PAGE_SIZE, PAGE_SIZE);
        pt[evict_pte] = 0;
        clearBitmap(&phys_bitmap, evict_ppn);
        clearBitmap(&virt_bitmap, evict_vpn);
        invalidateTLB((void*)evict_va);
    }
    unsigned long cur_page = 0;
    for (; cur_page < num_phys_pages; cur_page++) {
        if (!isBitmapSet(&phys_bitmap, cur_page)) break;
    }
    if (cur_page >= num_phys_pages) return -1;
    void* pa = (void*)(cur_page << offset_bits);
    if (pageMap(pgdir, va, pa) == -1) return -1;
    setBitmap(&phys_bitmap, cur_page);
    setBitmap(&virt_bitmap, vpn);
    queue_push(&page_queue, vpn);
    return 0;
}

pte_t* checkTLB(void* va) {

}

int addTLB(void* va, void* pa) {

}

void invalidateTLB(void* va) {
    unsigned long vpn = (unsigned long)va >> offset_bits;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb.entry[i].valid && tlb.entry[i].v_page == vpn) {
            tlb.entry[i].valid = false;
        }
    }
}