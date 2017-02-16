/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright (c) 2017, Tegile Systems, Inc. All rights reserved.
 */

/*
 * XXX This stuff should be in usr/src/common, to be shared by boot
 * code, kernel DR, and busra stuff.
 *
 * NOTE: We are only using the next-> link. The prev-> link is
 *	not used in the implementation.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/memlist.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/pci_impl.h>
#include <sys/debug.h>

extern int pci_boot_debug;
#define	dprintf if (pci_boot_debug) printf

void
memlist_dump(struct memlist *listp)
{
	dprintf("memlist 0x%p content", (void *)listp);
	while (listp) {
		dprintf("(0x%" PRIx64 ", 0x%" PRIx64 ")",
		    listp->ml_address, listp->ml_size);
		listp = listp->ml_next;
	}
}

struct memlist *
memlist_alloc()
{
	return ((struct memlist *)kmem_zalloc(sizeof (struct memlist),
	    KM_SLEEP));
}

void
memlist_free(struct memlist *buf)
{
	kmem_free(buf, sizeof (struct memlist));
}

void
memlist_free_all(struct memlist **list)
{
	struct memlist  *next, *buf;

	next = *list;
	while (next) {
		buf = next;
		next = buf->ml_next;
		kmem_free(buf, sizeof (struct memlist));
	}
	*list = NULL;
}

/*
 * Insert a new memlist. The ml_next chain is a standard null terminated
 * ordered list of memlists. The ml_prev chain is a circular list to
 * allow us to get to the last memlist from the head of the list.
 */
static void
memlist_insert_entry(struct memlist **listp, struct memlist *entry,
    struct memlist *prev, struct memlist *next)
{
	if (prev == NULL) {
		/* Inserting at the front */
		if (*listp == NULL) {
			/* this is the first entry in the list */
			entry->ml_prev = entry;
		} else {
			entry->ml_prev = (*listp)->ml_prev;
			(*listp)->ml_prev = entry;
		}
		entry->ml_next = *listp;
		*listp = entry;
	} else if (next == NULL) {
		/* Inserting at the end */
		(*listp)->ml_prev = entry;
		entry->ml_prev = prev;
		prev->ml_next = entry;
	} else {
		/* in the middle */
		entry->ml_prev = prev;
		next->ml_prev = entry;
		entry->ml_next = next;
		prev->ml_next = entry;
	}
}

static void
memlist_remove_entry(struct memlist **listp, struct memlist *entry,
    struct memlist *prev)
{
	if (prev == NULL) {
		/* removing the front entry */
		ASSERT(*listp == entry);
		if (entry->ml_next)
			entry->ml_next->ml_prev = entry->ml_prev;
		*listp = entry->ml_next;
	} else if (entry->ml_next == NULL) {
		/* last entry */
		(*listp)->ml_prev = prev;
		prev->ml_next = NULL;
	} else {
		/* a middle entry */
		entry->ml_next->ml_prev = entry->ml_prev;
		prev->ml_next = entry->ml_next;
	}
}

/* insert in the order of addresses */
void
memlist_insert(struct memlist **listp, uint64_t addr, uint64_t size)
{
	int merge_left, merge_right;
	struct memlist *entry;
	struct memlist *prev = NULL, *next;

	/* find the location in list */
	next = *listp;
	while (next && next->ml_address <= addr) {
		/*
		 * Drop if this entry already exists, in whole
		 * or in part
		 */
		if (next->ml_address <= addr &&
		    next->ml_address + next->ml_size >= addr + size) {
			/* next already contains this entire element; drop */
			return;
		}

		/* Is this a "grow block size" request? */
		if (next->ml_address == addr) {
			break;
		}
		prev = next;
		next = prev->ml_next;
	}

	merge_left = (prev && addr == prev->ml_address + prev->ml_size);
	merge_right = (next && addr + size == next->ml_address);
	if (merge_left && merge_right) {
		prev->ml_size += size + next->ml_size;
		memlist_remove_entry(listp, next, prev);
		memlist_free(next);
		return;
	}

	if (merge_left) {
		prev->ml_size += size;
		return;
	}

	if (merge_right) {
		next->ml_address = addr;
		next->ml_size += size;
		return;
	}

	entry = memlist_alloc();
	entry->ml_address = addr;
	entry->ml_size = size;
	memlist_insert_entry(listp, entry, prev, next);
}

/*
 * Delete memlist entries, assuming list sorted by address
 * returns 0 if delete successful, otherwise returns 1.
 */

#define	IN_RANGE(a, b, e) ((a) >= (b) && (a) <= (e))

int
memlist_remove(struct memlist **listp, uint64_t addr, uint64_t size)
{
	struct memlist *prev = NULL;
	struct memlist *chunk;
	uint64_t rem_begin, rem_end;
	uint64_t chunk_begin, chunk_end;
	int begin_in_chunk, end_in_chunk;
	int rv = 1;

	/* ignore removal of zero-length item */
	if (size == 0)
		return (0);

	/* also inherently ignore a zero-length list */
	rem_begin = addr;
	rem_end = addr + size - 1;
	chunk = *listp;
	while (chunk) {
		chunk_begin = chunk->ml_address;
		chunk_end = chunk->ml_address + chunk->ml_size - 1;
		begin_in_chunk = IN_RANGE(rem_begin, chunk_begin, chunk_end);
		end_in_chunk = IN_RANGE(rem_end, chunk_begin, chunk_end);

		if (rem_begin <= chunk_begin && rem_end >= chunk_end) {
			struct memlist *delete_chunk, *next;

			/* spans entire chunk - delete chunk */
			delete_chunk = chunk;
			next = chunk->ml_next;
			memlist_remove_entry(listp, delete_chunk, prev);
			chunk = prev ? next : *listp;
			memlist_free(delete_chunk);
			rv = 0;
			/* skip to start of while-loop */
			continue;
		} else if (begin_in_chunk && end_in_chunk &&
		    chunk_begin != rem_begin && chunk_end != rem_end) {
			struct memlist *new;
			/* split chunk */
			new = memlist_alloc();
			new->ml_address = rem_end + 1;
			new->ml_size = chunk_end - new->ml_address + 1;
			chunk->ml_size = rem_begin - chunk_begin;
			memlist_insert_entry(listp, new, chunk, chunk->ml_next);
			rv = 0;
			/* done - break out of while-loop */
			break;
		} else if (begin_in_chunk || end_in_chunk) {
			/* trim chunk */
			rv = 0;
			chunk->ml_size -= MIN(chunk_end, rem_end) -
			    MAX(chunk_begin, rem_begin) + 1;
			if (rem_begin <= chunk_begin) {
				chunk->ml_address = rem_end + 1;
				break;
			}
			/* fall-through to next chunk */
		}
		prev = chunk;
		chunk = chunk->ml_next;
	}

	return (rv);
}

/*
 * find and claim a 32bit memory chunk of given size, first fit
 */
uint64_t
memlist_find(struct memlist **listp, uint64_t size, uint64_t align)
{
	uint64_t delta, total_size;
	uint64_t paddr;
	struct memlist *prev = NULL, *next;

	ASSERT(ISP2(align));

	/* find the chunk with sufficient size */
	next = *listp;
	while (next && next->ml_address < UINT_MAX) {
		delta = P2ALIGN(next->ml_address, align) - next->ml_address;
		if (delta != 0)
			total_size = size + align - delta;
		else
			total_size = size; /* the addr is already aligned */
		if (next->ml_size >= total_size)
			break;
		prev = next;
		next = prev->ml_next;
	}

	if (next == NULL || next->ml_address >= UINT_MAX)
		return (0);	/* Not found */

	paddr = next->ml_address;
	if (delta)
		paddr += align - delta;

	if (paddr + size > UINT_MAX)
		return (0);

	(void) memlist_remove(listp, paddr, size);

	return (paddr);
}

/*
 * Find and claim a 64bit memory chunk of given size, first fit.
 * We search from high to low address.
 */
uint64_t
memlist_find64(struct memlist **listp, uint64_t size, uint64_t align)
{
	uint64_t paddr;
	struct memlist *prev = NULL, *entry;
	boolean_t found = B_FALSE;

	ASSERT(ISP2(align));

	if (*listp == NULL)
		return (0);

	/* find the chunk with sufficient size */
	entry = (*listp)->ml_prev;
	while (prev != *listp) {
		if (size <= entry->ml_size) {
			/* assign addresses from the end of the memlist */
			paddr = entry->ml_address + entry->ml_size - size;
			paddr = P2ALIGN(paddr, align);
			if (paddr >= entry->ml_address) {
				found = B_TRUE;
				break;
			}
		}

		prev = entry;
		/* go backwards */
		entry = entry->ml_prev;
	}

	if (!found)
		return (0);	/* Not found */

	(void) memlist_remove(listp, paddr, size);

	return (paddr);
}

/*
 * find and claim a memory chunk of given size, starting
 * at a specified address
 */
uint64_t
memlist_find_with_startaddr(struct memlist **listp, uint64_t address,
    uint64_t size, uint64_t align)
{
	uint64_t delta, total_size;
	uint64_t paddr;
	struct memlist *next;

	/* find the chunk starting at 'address' */
	next = *listp;
	while (next && (next->ml_address != address)) {
		next = next->ml_next;
	}
	if (next == NULL)
		return (0);	/* Not found */

	delta = next->ml_address & ((align != 0) ? (align - 1) : 0);
	if (delta != 0)
		total_size = size + align - delta;
	else
		total_size = size;	/* the addr is already aligned */
	if (next->ml_size < total_size)
		return (0);	/* unsufficient size */

	paddr = next->ml_address;
	if (delta)
		paddr += align - delta;
	(void) memlist_remove(listp, paddr, size);

	return (paddr);
}

/*
 * Subsume memlist src into memlist dest
 */
void
memlist_subsume(struct memlist **src, struct memlist **dest)
{
	struct memlist *head, *prev;

	head = *src;
	while (head) {
		memlist_insert(dest, head->ml_address, head->ml_size);
		prev = head;
		head = head->ml_next;
		memlist_free(prev);
	}
	*src = NULL;
}

/*
 * Merge memlist src into memlist dest; don't destroy src
 */
void
memlist_merge(struct memlist **src, struct memlist **dest)
{
	struct memlist *p;

	p = *src;
	while (p) {
		memlist_insert(dest, p->ml_address, p->ml_size);
		p = p->ml_next;
	}
}

/*
 * Make a copy of memlist
 */
struct memlist *
memlist_dup(struct memlist *listp)
{
	struct memlist *head = NULL, *prev = NULL;

	while (listp) {
		struct memlist *entry = memlist_alloc();
		entry->ml_address = listp->ml_address;
		entry->ml_size = listp->ml_size;
		memlist_insert_entry(&head, entry, prev, NULL);
		prev = entry;
		listp = listp->ml_next;
	}

	return (head);
}

int
memlist_count(struct memlist *listp)
{
	int count = 0;
	while (listp) {
		count++;
		listp = listp->ml_next;
	}

	return (count);
}
