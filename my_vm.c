#include "my_vm.h"

#define get_pde_bits(x, pde_width) ((x) >> (ADDRESS_BITS - pde_width))
#define get_pte_bits(x, pte_width, offset) ((x >> (offset)) & ((1UL << (pte_width)) - 1))
#define get_offset_bits(x, offset) ((x) & ((1UL << num) - 1))
static Bitmap phys_bitmap;
static Bitmap virt_bitmap;
static pde_t* page_directory;
static TLB tlb;
static unsigned long num_phys_pages;
static unsigned long num_virt_pages;
static char* physical_memory;
static char* disk;

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
    page_directory = (pde_t*)malloc(PAGE_SIZE);
    memset(page_directory, 0, PAGE_SIZE);
    memset(&tlb, 0, sizeof(TLB));
    physical_memory = (char*)malloc(PM_SIZE);
    disk = (char*)malloc(DISK_SIZE);
    //HINT: Also calculate the number of physical and virtual pages and allocate
    //virtual and physical bitmaps and initialize them

}



/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
pte_t* translate(pde_t* pgdir, void* va) {
    //HINT: Get the Page directory index (1st level) Then get the
    //2nd-level-page table index using the virtual address.  Using the page
    //directory index and page table index get the physical address
    int offset_bits = log2(PAGE_SIZE);
    int pde_bits = (ADDRESS_BITS - offset_bits) / 2;
    int pte_bits = ADDRESS_BITS - offset_bits - pde_bits;
    unsigned long va_num = (unsigned long)va;
    unsigned long pde_index = get_pde_bits(va_num, pde_bits);
    unsigned long pte_index = get_pte_bits(va_num, pte_bits, offset_bits);
    unsigned long offset = get_offset_bits(va_num, offset_bits);
    if (pde_index >= (1UL << pde_bits)) return NULL;
    pde_t pde = pgdir[pde_index];
    if (!(pde & 0x1)) return NULL;
    pte_t* page_table = (pte_t*)(pde & ~0xFFFUL);
    if (pte_index >= (1UL << pte_bits)) return NULL;
    pte_t pte = page_table[pte_index];
    if (!(pte & 0x1)) return NULL;
    unsigned long p_page = pte & ~0xFFFUL;
    return (pte_t*)(p_page + offset);
    //If translation not successfull
    return NULL;
}


/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int
pageMap(pde_t* pgdir, void* va, void* pa) {

    /*HINT: Similar to translate(), find the page directory (1st level)
    and page table (2nd-level) indices. If no mapping exists, set the
    virtual to physical mapping */

    return -1;
}


/* Function responsible for allocating pages
and used by the benchmark
*/
void* myMalloc(unsigned int num_bytes) {

    //HINT: If the physical memory is not yet initialized, then allocate and initialize.

   /* HINT: If the page directory is not initialized, then initialize the
   page directory. Next, using get_next_avail(), check if there are free pages. If
   free pages are available, set the bitmaps and map a new page. Note, you will
   have to mark which physical pages are used. */

    return NULL;
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
