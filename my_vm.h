#ifndef __MY_VM_H__
#define __MY_VM_H__

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "defines.h"

/*

Illustration of a 2-level page table with 4KB pages.
In this example, lower 12 bits are used for page offset, and rest of
the bits are divided equally into 2 levels.

 31      22 21      12 11        0
+----------+----------+----------+
| PDE (L2) | PTE (L1) |  OFFSET  |
+----------+----------+----------+

*/

// Represents a page directory entry
typedef unsigned long pde_t;

// Represents a page table entry
typedef unsigned long pte_t;

// TLB data structure example. Feel free to modify
typedef struct {
	struct {
		bool valid;  // valid bit
		unsigned long v_page;  // virtual page number
		unsigned long p_page;  // physical page number
	} entry[TLB_SIZE];
	unsigned int tlb_accesses;
	unsigned int tlb_misses;
} TLB;

typedef struct {
	unsigned long num_pages; // Total number of pages
	unsigned char* bitmap;   // Bitmap to track allocated pages
	unsigned long free_pages; // Number of free pages
} Bitmap;

void initBitmap(Bitmap* bitmap, unsigned long num_pages) {
	bitmap->num_pages = num_pages;
	bitmap->bitmap = (unsigned char*)calloc((num_pages + 7) / 8, sizeof(unsigned char));
	bitmap->free_pages = num_pages;
}
void setBitmap(Bitmap* bitmap, unsigned long page_num) {
	bitmap->bitmap[page_num / 8] |= (1 << (page_num % 8));
	bitmap->free_pages--;
}
void clearBitmap(Bitmap* bitmap, unsigned long page_num) {
	bitmap->bitmap[page_num / 8] &= ~(1 << (page_num % 8));
	bitmap->free_pages++;
}
bool isBitmapSet(Bitmap* bitmap, unsigned long page_num) {
	return (bitmap->bitmap[page_num / 8] & (1 << (page_num % 8))) != 0;
}

typedef struct {
	struct node* next;
	unsigned long data;
}node;

typedef struct {
	node* head;
	node* tail;
}queue;

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
	} else {
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


void initMemoryAndDisk();
pte_t* translate(pde_t* pgdir, void* va);
int pageMap(pde_t* pgdir, void* va, void* pa);
int pageFault(pde_t* pgdir, void* va);
pte_t* checkTLB(void* va);
int addTLB(void* va, void* pa);
void invalidateTLB(void* va);

void myFree(void* va, int size);
void* myMalloc(unsigned int num_bytes);
void myWrite(void* va, void* val, int size);
void myRead(void* va, void* val, int size);

#endif
