// SPDX-License-Identifier: MIT
/*
 * Minimal intrusive doubly linked list, API-compatible with the subset of the
 * Linux kernel list interface used by this project.
 */

#ifndef BTRFS_RO_LIST_H
#define BTRFS_RO_LIST_H

#include <stddef.h>
#include <stdbool.h>

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static inline void __list_link(struct list_head *entry, struct list_head *prev,
			       struct list_head *next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

static inline void list_add(struct list_head *entry, struct list_head *head)
{
	__list_link(entry, head, head->next);
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	__list_link(entry, head->prev, head);
}

static inline void list_del_init(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	INIT_LIST_HEAD(entry);
}

static inline void list_del(struct list_head *entry)
{
	list_del_init(entry);
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

#define list_entry(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#define list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)

#define list_last_entry(head, type, member) \
	list_entry((head)->prev, type, member)

#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, __typeof__(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_entry((head)->next, __typeof__(*pos), member),	\
	     n = list_entry(pos->member.next, __typeof__(*pos), member);	\
	     &pos->member != (head);					\
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

#endif
