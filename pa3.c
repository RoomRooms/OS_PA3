/**********************************************************************
 * Copyright (c) 2020-2022
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "types.h"
#include "list_head.h"
#include "vm.h"

/**
 * Ready queue of the system
 */
extern struct list_head processes;

/**
 * Currently running process
 */
extern struct process *current;

/**
 * Page Table Base Register that MMU will walk through for address translation
 */
extern struct pagetable *ptbr;

/**
 * TLB of the system.
 */
extern struct tlb_entry tlb[1UL << (PTES_PER_PAGE_SHIFT * 2)];


/**
 * The number of mappings for each page frame. Can be used to determine how
 * many processes are using the page frames.
 */
extern unsigned int mapcounts[];


/**
 * lookup_tlb(@vpn, @pfn)
 *
 * DESCRIPTION
 *   Translate @vpn of the current process through TLB. DO NOT make your own
 *   data structure for TLB, but use the defined @tlb data structure
 *   to translate. If the requested VPN exists in the TLB, return true
 *   with @pfn is set to its PFN. Otherwise, return false.
 *   The framework calls this function when needed, so do not call
 *   this function manually.
 *
 * RETURN
 *   Return true if the translation is cached in the TLB.
 *   Return false otherwise
 */
bool lookup_tlb(unsigned int vpn, unsigned int *pfn)
{
	for (int i = 0; i < sizeof(tlb) / sizeof(*tlb); i++) {
		struct tlb_entry *t = tlb + i;

		if (!t->valid) continue;
		else
			if(t->vpn == vpn){
				*pfn = t->pfn;
				return true;
			}
	}
	return false;
}


/**
 * insert_tlb(@vpn, @pfn)
 *
 * DESCRIPTION
 *   Insert the mapping from @vpn to @pfn into the TLB. The framework will call
 *   this function when required, so no need to call this function manually.
 *
 */
void insert_tlb(unsigned int vpn, unsigned int pfn)
{
	for (int i = 0; i < sizeof(tlb) / sizeof(*tlb); i++) {
		struct tlb_entry *t = tlb + i;

		if (!t->valid){
			t->valid = true;
			t->vpn = vpn;
			t->pfn = pfn;
			break;
		}
	}
}


/**
 * alloc_page(@vpn, @rw)
 *
 * DESCRIPTION
 *   Allocate a page frame that is not allocated to any process, and map it
 *   to @vpn. When the system has multiple free pages, this function should
 *   allocate the page frame with the **smallest pfn**.
 *   You may construct the page table of the @current process. When the page
 *   is allocated with RW_WRITE flag, the page may be later accessed for writes.
 *   However, the pages populated with RW_READ only should not be accessed with
 *   RW_WRITE accesses.
 *
 * RETURN
 *   Return allocated page frame number.
 *   Return -1 if all page frames are allocated.
 */
unsigned int alloc_page(unsigned int vpn, unsigned int rw)
{
	int pd_index = vpn / NR_PTES_PER_PAGE;
	int pte_index = vpn % NR_PTES_PER_PAGE;
	
	struct pagetable *pt = ptbr;
	struct pte_directory *pd;
	struct pte *pte;
	
	if (!pt){
		fprintf(stderr,"Page Table is NULL!\n");
		return -1;
	}
	pd = pt->outer_ptes[pd_index];
	
	if (!pd){
		pt->outer_ptes[pd_index] = (struct pte_directory*)malloc(sizeof(struct pte_directory));
		pd = pt->outer_ptes[pd_index];
	}

	pte = &pd->ptes[pte_index];

	pte->valid = true;
	pte->private = 0;
	
	if(rw >= RW_WRITE){
		pte->writable = true;
	}
	
	pte->pfn = -1;
		
	for(unsigned int i=0;i<NR_PAGEFRAMES;i++)
		if(!mapcounts[i]){
			pte->pfn = i;
			mapcounts[i]++;
			break;
		}
	
	return pte->pfn;
}


/**
 * free_page(@vpn)
 *
 * DESCRIPTION
 *   Deallocate the page from the current processor. Make sure that the fields
 *   for the corresponding PTE (valid, writable, pfn) is set @false or 0.
 *   Also, consider carefully for the case when a page is shared by two processes,
 *   and one process is to free the page.
 */
void free_page(unsigned int vpn)
{
	int pd_index = vpn / NR_PTES_PER_PAGE;
	int pte_index = vpn % NR_PTES_PER_PAGE;
	bool flag = false;
	
	struct pagetable *pt = ptbr;
	struct pte_directory *pd;
	struct pte *pte;
	
	pd = pt->outer_ptes[pd_index];

	pte = &pd->ptes[pte_index];
	
	for (int i = 0; i < sizeof(tlb) / sizeof(*tlb); i++) {
		struct tlb_entry *t = tlb + i;

		if (!t->valid) continue;
		else
			if(t->vpn == vpn){
				t->valid = false;
				t->pfn = 0;
				t->vpn = 0;
			}
	}	
	
	if(mapcounts[pte->pfn]>0){
		mapcounts[pte->pfn]--;
		pte->valid=false;
		pte->private=0;
		pte->writable=false;
		pte->pfn=0;
	}
	
	for(int i=0;i<NR_PTES_PER_PAGE;i++){
		if(!pd->ptes[i].valid) continue;
		else{
			flag=true;
			break;
		}
	}
	
	if(!flag)
		free(pd);
}


/**
 * handle_page_fault()
 *
 * DESCRIPTION
 *   Handle the page fault for accessing @vpn for @rw. This function is called
 *   by the framework when the __translate() for @vpn fails. This implies;
 *   0. page directory is invalid
 *   1. pte is invalid
 *   2. pte is not writable but @rw is for write
 *   This function should identify the situation, and do the copy-on-write if
 *   necessary.
 *
 * RETURN
 *   @true on successful fault handling
 *   @false otherwise
 */
bool handle_page_fault(unsigned int vpn, unsigned int rw)
{	
	if(rw == RW_WRITE){
		int pd_index = vpn / NR_PTES_PER_PAGE;
		int pte_index = vpn % NR_PTES_PER_PAGE;
		
		struct pagetable *pt = ptbr;
		struct pte_directory *pd;
		struct pte *pte;
		
		pd = pt->outer_ptes[pd_index];

		pte = &pd->ptes[pte_index];
		
		if(pte->private==1){
			pte->writable = true;
			mapcounts[pte->pfn]--;
			
			for(unsigned int i=0;i<NR_PAGEFRAMES;i++)
				if(!mapcounts[i]){
					pte->pfn = i;
					mapcounts[i]++;
					return true;
				}
		}
	}
	return false;
}


/**
 * switch_process()
 *
 * DESCRIPTION
 *   If there is a process with @pid in @processes, switch to the process.
 *   The @current process at the moment should be put into the @processes
 *   list, and @current should be replaced to the requested process.
 *   Make sure that the next process is unlinked from the @processes, and
 *   @ptbr is set properly.
 *
 *   If there is no process with @pid in the @processes list, fork a process
 *   from the @current. This implies the forked child process should have
 *   the identical page table entry 'values' to its parent's (i.e., @current)
 *   page table. 
 *   To implement the copy-on-write feature, you should manipulate the writable
 *   bit in PTE and mapcounts for shared pages. You may use pte->private for 
 *   storing some useful information :-)
 */
void switch_process(unsigned int pid)
{
	struct process *a = NULL;
	struct pte_directory *pd, *npd;
	struct pte *pte, *npte;
	
	list_for_each_entry(a, &processes, list){
		if(a->pid == pid)
			break;
	}
	
	if(a->pid != pid) a = NULL;
		
	if(!a){
		struct process *new = (struct process*)malloc(sizeof(struct process));
		new->pid = pid;
		
		for (int i = 0; i < NR_PTES_PER_PAGE; i++) {
			pd = current->pagetable.outer_ptes[i];

			if (!pd) continue;
			else {
				new->pagetable.outer_ptes[i] = (struct pte_directory*)malloc(sizeof(struct pte_directory));
				npd = new->pagetable.outer_ptes[i];
				
				for (int j = 0; j < NR_PTES_PER_PAGE; j++) {
					pte = &pd->ptes[j];
					if (!pte->valid) continue;
					
					npte = &npd->ptes[j];
					npte->pfn = pte->pfn;
					npte->private = pte->private;
					npte->valid = true;
					if(pte->writable){
						npte->private = 1;
						pte->private = 1;
						pte->writable = false;
					}
					
					mapcounts[pte->pfn]++;
				}
				
			}
		}	
		
		list_add(&current->list, &processes);
		current = new;
		ptbr = &new->pagetable;
		for (int i = 0; i < sizeof(tlb) / sizeof(*tlb); i++) {
			struct tlb_entry *t = tlb + i;

			if (!t->valid) continue;
			else{
				t->valid = false;
				t->pfn = 0;
				t->vpn = 0;
			}
		}
	}else{
		for (int i = 0; i < NR_PTES_PER_PAGE; i++) {
			pd = current->pagetable.outer_ptes[i];

			if (!pd) continue;
			else {
				for (int j = 0; j < NR_PTES_PER_PAGE; j++) {
					pte = &pd->ptes[j];
					if (!pte->valid) continue;
					
					if(pte->writable){
						pte->private = 1;
						pte->writable = false;
					}
					
				}
				
			}
		}
		list_add(&current->list, &processes);
		list_del(&a->list);
		current = a;
		ptbr = &a->pagetable;
		
		for (int i = 0; i < sizeof(tlb) / sizeof(*tlb); i++) {
			struct tlb_entry *t = tlb + i;

			if (!t->valid) continue;
			else{
				t->valid = false;
				t->pfn = 0;
				t->vpn = 0;
			}
		}
	}
}

