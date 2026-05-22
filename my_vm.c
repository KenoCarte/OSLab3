#include "my_vm.h"

#define get_pde_bits(x, pde_width) ((x) >> (ADDRESS_BITS - pde_width))
#define get_pte_bits(x, pte_width, offset) ((x >> (offset)) & ((1UL << (pte_width)) - 1))
#define get_offset_bits(x, offset_bits) ((x) & ((1UL << (offset_bits)) - 1))
// phys_bitmap: 跟踪物理页的占用情况（位图）
// virt_bitmap: 跟踪虚拟页的占用情况（位图）
// page_directory: 顶层页目录（two-level page table 的第一层），存放页表的基地址
// tlb: 快表（TLB）缓存用于加速虚拟地址到物理地址的转换
// num_phys_pages / num_virt_pages: 物理/虚拟页总数
// physical_memory: 仿真的物理内存空间（连续字节数组）
// disk: 仿真的二级存储（页面换出到这里）
// is_initialized: 初始化标志
// vm_mutex: 保护虚拟内存管理数据结构的互斥锁，避免并发竞争
// page_queue: 记录页的使用顺序，用于实现简单的页面置换（FIFO）
static Bitmap phys_bitmap;
static Bitmap virt_bitmap;
static pde_t* page_directory;
static TLB tlb;
static unsigned long num_phys_pages;
static unsigned long num_virt_pages;
static char* physical_memory;
static char* disk;
static int is_initialized = 0;
static pthread_mutex_t vm_mutex = PTHREAD_MUTEX_INITIALIZER;
queue page_queue;
const int offset_bits = log2(PAGE_SIZE);
const int pde_bits = (ADDRESS_BITS - offset_bits) / 2;
const int pte_bits = ADDRESS_BITS - offset_bits - pde_bits;

// 初始化位图：设置页数、分配用于记录位的字节数组、以及空闲页计数
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

// 队列用于记录页面顺序，以便页面置换
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
说明：该函数懒初始化仿真所需的结构（位图、页目录、模拟内存/磁盘等），避免重复分配。
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
说明：translate 尝试先从 TLB 命中获取映射；若未命中则访问页目录/页表，遇到缺页触发 pageFault。
返回值：指向物理内存中对应字节的指针（pte_t* 仅作为字节指针使用）
*/
pte_t* translate(pde_t* pgdir, void* va) {
    pte_t* tlb_phys = checkTLB(va);
    if (tlb_phys) {
        tlb.tlb_accesses++;
        return tlb_phys;
    }
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
    // pte 存储的是物理地址（页对齐的基地址），加上偏移得到物理内存中的字节地址
    pte_t* pt_phys = (pte_t*)(physical_memory + pte + offset);
    // Always use page table as source of truth; TLB as cache
    tlb.tlb_misses++;
    addTLB(va, (void*)pte);
    return pt_phys;
}

/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
说明：pageMap 负责在页目录/页表中设置从虚拟页到物理页的映射（页对齐地址）。
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
    page_table[pte_index] = (unsigned long)pa; // 存储物理地址（页对齐）
    return 0;
}

/* Function responsible for allocating pages
and used by the benchmark
说明：myMalloc 在虚拟地址空间中寻找连续的空闲虚拟页区间，
为每个虚拟页分配一个空闲物理页（若物理页不足则失败或触发置换），
并在页表中建立映射，返回分配到的虚拟地址基址（页对齐）。
*/
void* myMalloc(unsigned int num_bytes) {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    unsigned int num_pages = (num_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages == 0) { pthread_mutex_unlock(&vm_mutex); return NULL; }
    unsigned long start_vpage = 1;
    unsigned int found = 0;
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
    unsigned long cur_page = 1;
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
    pthread_mutex_unlock(&vm_mutex);
    return result;
}

/* Responsible for releasing one or more memory pages using virtual address (va)
说明：myFree 通过虚拟地址撤销映射、释放物理页并清理页表条目，
若页表变为空则释放页表。对 TLB 做失效处理。
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
    pthread_mutex_unlock(&vm_mutex);
}

/* The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
说明：myWrite 会按页边界循环调用 translate 获取物理指针并写入数据，
若访问到未分配地址则报错并返回。
*/
void myWrite(void* va, void* val, int size) {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    while (size) {
        pte_t* phys = translate(page_directory, va);
        if (!phys || (char*)phys < physical_memory || (char*)phys >= physical_memory + PM_SIZE) {
            printf("ERROR: Writing to unallocated address\n");
            pthread_mutex_unlock(&vm_mutex);
            return;
        }
        unsigned long offset = (unsigned long)va & (PAGE_SIZE - 1);
        unsigned long to_write = PAGE_SIZE - offset < size ? PAGE_SIZE - offset : size;
        // 将数据拷贝到物理内存对应位置（物理指针由 translate 返回）
        memcpy(phys, val, to_write);
        size -= to_write;
        val = (char*)val + to_write;
        va = (char*)va + to_write;
    }
    pthread_mutex_unlock(&vm_mutex);
}

/*Given a virtual address, this function copies the contents of the page to val
说明：myRead 按页边界读取数据，同样通过 translate 获取物理地址。
*/
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

// pageFault: 处理缺页，尝试为 vpn 分配物理页，必要时进行置换并将页面写回磁盘
int pageFault(pde_t* pgdir, void* va) {
    unsigned long va_num = sanitizeVA(va);
    unsigned long vpn = va_num >> offset_bits;
    unsigned long cur_page = 0;
    if (phys_bitmap.free_pages == 0) {
        if (!page_queue.head) return -1; // 没有可置换的页
        unsigned long evict_vpn = queue_pop(&page_queue);
        unsigned long evict_va = evict_vpn << offset_bits;
        unsigned long evict_pde = get_pde_bits(evict_va, pde_bits);
        unsigned long evict_pte = get_pte_bits(evict_va, pte_bits, offset_bits);
        pte_t* pt = (pte_t*)pgdir[evict_pde];
        unsigned long evict_ppn = pt[evict_pte] >> offset_bits;
        // 将被置换页面写入模拟磁盘
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

// TLB 相关函数：检查、添加和失效
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

// findnext: 选择下一个 TLB 替换槽位（简单的基于访问时间的替换策略）
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

void printTLBStats() {
    pthread_mutex_lock(&vm_mutex);
    initMemoryAndDisk();
    unsigned int total = tlb.tlb_accesses + tlb.tlb_misses;
    printf("TLB Total Accesses: %u\n", total);
    if (total > 0) {
        printf("TLB Hit Rate: %.4f%%\n", (double)tlb.tlb_accesses / total * 100);
    }
    pthread_mutex_unlock(&vm_mutex);
}