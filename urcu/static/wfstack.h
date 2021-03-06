#ifndef _URCU_STATIC_WFSTACK_H
#define _URCU_STATIC_WFSTACK_H

/*
 * urcu/static/wfstack.h
 *
 * Userspace RCU library - Stack with with wait-free push, blocking traversal.
 *
 * TO BE INCLUDED ONLY IN LGPL-COMPATIBLE CODE. See urcu/wfstack.h for
 * linking dynamically with the userspace rcu library.
 *
 * Copyright 2010-2012 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>
#include <assert.h>
#include <poll.h>
#include <stdbool.h>
#include <urcu/compiler.h>
#include <urcu/uatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDS_WFS_END			((void *) 0x1UL)
#define CDS_WFS_ADAPT_ATTEMPTS		10	/* Retry if being set */
#define CDS_WFS_WAIT			10	/* Wait 10 ms if being set */

/*
 * Stack with wait-free push, blocking traversal.
 *
 * Stack implementing push, pop, pop_all operations, as well as iterator
 * on the stack head returned by pop_all.
 *
 * Wait-free operations: cds_wfs_push, __cds_wfs_pop_all.
 * Blocking operations: cds_wfs_pop, cds_wfs_pop_all, iteration on stack
 *                      head returned by pop_all.
 *
 * Synchronization table:
 *
 * External synchronization techniques described in the API below is
 * required between pairs marked with "X". No external synchronization
 * required between pairs marked with "-".
 *
 *                      cds_wfs_push  __cds_wfs_pop  __cds_wfs_pop_all
 * cds_wfs_push               -              -                  -
 * __cds_wfs_pop              -              X                  X
 * __cds_wfs_pop_all          -              X                  -
 *
 * cds_wfs_pop and cds_wfs_pop_all use an internal mutex to provide
 * synchronization.
 */

/*
 * cds_wfs_node_init: initialize wait-free stack node.
 */
static inline
void _cds_wfs_node_init(struct cds_wfs_node *node)
{
	node->next = NULL;
}

/*
 * cds_wfs_init: initialize wait-free stack.
 */
static inline
void _cds_wfs_init(struct cds_wfs_stack *s)
{
	int ret;

	s->head = CDS_WFS_END;
	ret = pthread_mutex_init(&s->lock, NULL);
	assert(!ret);
}

static inline bool ___cds_wfs_end(void *node)
{
	return node == CDS_WFS_END;
}

/*
 * cds_wfs_empty: return whether wait-free stack is empty.
 *
 * No memory barrier is issued. No mutual exclusion is required.
 */
static inline bool _cds_wfs_empty(struct cds_wfs_stack *s)
{
	return ___cds_wfs_end(CMM_LOAD_SHARED(s->head));
}

/*
 * cds_wfs_push: push a node into the stack.
 *
 * Issues a full memory barrier before push. No mutual exclusion is
 * required.
 *
 * Returns 0 if the stack was empty prior to adding the node.
 * Returns non-zero otherwise.
 */
static inline
int _cds_wfs_push(struct cds_wfs_stack *s, struct cds_wfs_node *node)
{
	struct cds_wfs_head *old_head, *new_head;

	assert(node->next == NULL);
	new_head = caa_container_of(node, struct cds_wfs_head, node);
	/*
	 * uatomic_xchg() implicit memory barrier orders earlier stores
	 * to node (setting it to NULL) before publication.
	 */
	old_head = uatomic_xchg(&s->head, new_head);
	/*
	 * At this point, dequeuers see a NULL node->next, they should
	 * busy-wait until node->next is set to old_head.
	 */
	CMM_STORE_SHARED(node->next, &old_head->node);
	return !___cds_wfs_end(old_head);
}

/*
 * Waiting for push to complete enqueue and return the next node.
 */
static inline struct cds_wfs_node *
___cds_wfs_node_sync_next(struct cds_wfs_node *node)
{
	struct cds_wfs_node *next;
	int attempt = 0;

	/*
	 * Adaptative busy-looping waiting for push to complete.
	 */
	while ((next = CMM_LOAD_SHARED(node->next)) == NULL) {
		if (++attempt >= CDS_WFS_ADAPT_ATTEMPTS) {
			poll(NULL, 0, CDS_WFS_WAIT);	/* Wait for 10ms */
			attempt = 0;
		} else {
			caa_cpu_relax();
		}
	}

	return next;
}

/*
 * __cds_wfs_pop_blocking: pop a node from the stack.
 *
 * Returns NULL if stack is empty.
 *
 * __cds_wfs_pop_blocking needs to be synchronized using one of the
 * following techniques:
 *
 * 1) Calling __cds_wfs_pop_blocking under rcu read lock critical
 *    section. The caller must wait for a grace period to pass before
 *    freeing the returned node or modifying the cds_wfs_node structure.
 * 2) Using mutual exclusion (e.g. mutexes) to protect
 *     __cds_wfs_pop_blocking and __cds_wfs_pop_all callers.
 * 3) Ensuring that only ONE thread can call __cds_wfs_pop_blocking()
 *    and __cds_wfs_pop_all(). (multi-provider/single-consumer scheme).
 */
static inline
struct cds_wfs_node *
___cds_wfs_pop_blocking(struct cds_wfs_stack *s)
{
	struct cds_wfs_head *head, *new_head;
	struct cds_wfs_node *next;

	for (;;) {
		head = CMM_LOAD_SHARED(s->head);
		if (___cds_wfs_end(head))
			return NULL;
		next = ___cds_wfs_node_sync_next(&head->node);
		new_head = caa_container_of(next, struct cds_wfs_head, node);
		if (uatomic_cmpxchg(&s->head, head, new_head) == head)
			return &head->node;
		/* busy-loop if head changed under us */
	}
}

/*
 * __cds_wfs_pop_all: pop all nodes from a stack.
 *
 * __cds_wfs_pop_all does not require any synchronization with other
 * push, nor with other __cds_wfs_pop_all, but requires synchronization
 * matching the technique used to synchronize __cds_wfs_pop_blocking:
 *
 * 1) If __cds_wfs_pop_blocking is called under rcu read lock critical
 *    section, both __cds_wfs_pop_blocking and cds_wfs_pop_all callers
 *    must wait for a grace period to pass before freeing the returned
 *    node or modifying the cds_wfs_node structure. However, no RCU
 *    read-side critical section is needed around __cds_wfs_pop_all.
 * 2) Using mutual exclusion (e.g. mutexes) to protect
 *     __cds_wfs_pop_blocking and __cds_wfs_pop_all callers.
 * 3) Ensuring that only ONE thread can call __cds_wfs_pop_blocking()
 *    and __cds_wfs_pop_all(). (multi-provider/single-consumer scheme).
 */
static inline
struct cds_wfs_head *
___cds_wfs_pop_all(struct cds_wfs_stack *s)
{
	struct cds_wfs_head *head;

	/*
	 * Implicit memory barrier after uatomic_xchg() matches implicit
	 * memory barrier before uatomic_xchg() in cds_wfs_push. It
	 * ensures that all nodes of the returned list are consistent.
	 * There is no need to issue memory barriers when iterating on
	 * the returned list, because the full memory barrier issued
	 * prior to each uatomic_cmpxchg, which each write to head, are
	 * taking care to order writes to each node prior to the full
	 * memory barrier after this uatomic_xchg().
	 */
	head = uatomic_xchg(&s->head, CDS_WFS_END);
	if (___cds_wfs_end(head))
		return NULL;
	return head;
}

/*
 * cds_wfs_pop_lock: lock stack pop-protection mutex.
 */
static inline void _cds_wfs_pop_lock(struct cds_wfs_stack *s)
{
	int ret;

	ret = pthread_mutex_lock(&s->lock);
	assert(!ret);
}

/*
 * cds_wfs_pop_unlock: unlock stack pop-protection mutex.
 */
static inline void _cds_wfs_pop_unlock(struct cds_wfs_stack *s)
{
	int ret;

	ret = pthread_mutex_unlock(&s->lock);
	assert(!ret);
}

/*
 * Call __cds_wfs_pop_blocking with an internal pop mutex held.
 */
static inline
struct cds_wfs_node *
_cds_wfs_pop_blocking(struct cds_wfs_stack *s)
{
	struct cds_wfs_node *retnode;

	_cds_wfs_pop_lock(s);
	retnode = ___cds_wfs_pop_blocking(s);
	_cds_wfs_pop_unlock(s);
	return retnode;
}

/*
 * Call __cds_wfs_pop_all with an internal pop mutex held.
 */
static inline
struct cds_wfs_head *
_cds_wfs_pop_all_blocking(struct cds_wfs_stack *s)
{
	struct cds_wfs_head *rethead;

	_cds_wfs_pop_lock(s);
	rethead = ___cds_wfs_pop_all(s);
	_cds_wfs_pop_unlock(s);
	return rethead;
}

/*
 * cds_wfs_first_blocking: get first node of a popped stack.
 *
 * Content written into the node before enqueue is guaranteed to be
 * consistent, but no other memory ordering is ensured.
 *
 * Used by for-like iteration macros in urcu/wfstack.h:
 * cds_wfs_for_each_blocking()
 * cds_wfs_for_each_blocking_safe()
 */
static inline struct cds_wfs_node *
_cds_wfs_first_blocking(struct cds_wfs_head *head)
{
	if (___cds_wfs_end(head))
		return NULL;
	return &head->node;
}

/*
 * cds_wfs_next_blocking: get next node of a popped stack.
 *
 * Content written into the node before enqueue is guaranteed to be
 * consistent, but no other memory ordering is ensured.
 *
 * Used by for-like iteration macros in urcu/wfstack.h:
 * cds_wfs_for_each_blocking()
 * cds_wfs_for_each_blocking_safe()
 */
static inline struct cds_wfs_node *
_cds_wfs_next_blocking(struct cds_wfs_node *node)
{
	struct cds_wfs_node *next;

	next = ___cds_wfs_node_sync_next(node);
	if (___cds_wfs_end(next))
		return NULL;
	return next;
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_STATIC_WFSTACK_H */
