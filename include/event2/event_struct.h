/*
 * Copyright (c) 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright (c) 2007-2012 Niels Provos and Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef EVENT2_EVENT_STRUCT_H_INCLUDED_
#define EVENT2_EVENT_STRUCT_H_INCLUDED_

/** @file event2/event_struct.h
  Structures used by event.h.  Using these structures directly WILL harm
  forward compatibility: be careful.
  No field declared in this file should be used directly in user code.  Except
  for historical reasons, these fields would not be exposed at all.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <event2/event-config.h>
#ifdef EVENT__HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

/* For int types. */
#include <event2/util.h>

/* For evkeyvalq */
#include <event2/keyvalq_struct.h>

#define EVLIST_TIMEOUT	    0x01
#define EVLIST_INSERTED	    0x02 /* 事件已经插入到事件列表中 */
#define EVLIST_SIGNAL	    0x04
#define EVLIST_ACTIVE	    0x08 /* 事件处于激活状态 */
#define EVLIST_INTERNAL	    0x10 /* 内部事件, 忽略 */
#define EVLIST_ACTIVE_LATER 0x20
#define EVLIST_FINALIZING   0x40 
#define EVLIST_INIT	        0x80 /* 事件已经初始化 */

#define EVLIST_ALL          0xff

/* Fix so that people don't have to run with <sys/queue.h> */
#ifndef TAILQ_ENTRY
#define EVENT_DEFINED_TQENTRY_
#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;	/* next element */			\
	struct type **tqe_prev;	/* address of previous next element */	\
}
#endif /* !TAILQ_ENTRY */

#ifndef TAILQ_HEAD
#define EVENT_DEFINED_TQHEAD_
#define TAILQ_HEAD(name, type)			\
struct name {					\
	struct type *tqh_first;			\
	struct type **tqh_last;			\
}
#endif

/* Fix so that people don't have to run with <sys/queue.h> */
#ifndef LIST_ENTRY
#define EVENT_DEFINED_LISTENTRY_
#define LIST_ENTRY(type)						\
struct {								\
	struct type *le_next;	/* next element */			\
	struct type **le_prev;	/* address of previous next element */	\
}
#endif /* !LIST_ENTRY */

#ifndef LIST_HEAD
#define EVENT_DEFINED_LISTHEAD_
#define LIST_HEAD(name, type)						\
struct name {								\
	struct type *lh_first;  /* first element */			\
	}
#endif /* !LIST_HEAD */

struct event;

struct event_callback {
	TAILQ_ENTRY(event_callback) evcb_active_next;
	short evcb_flags;
	ev_uint8_t evcb_pri;	/* smaller numbers are higher priority, 值越小, 
优先级越高 */
	ev_uint8_t evcb_closure;/* 
作用是告之事件处理时调用不同的处理函数 */
                            /* evcb_closure 可被赋予的枚举值, 比如 
EV_CLOSURE_EVENT_SIGNAL  */
    
	/* allows us to adopt for different types of events */
        union {
		void (*evcb_callback)(evutil_socket_t, short, void *);
		void (*evcb_selfcb)(struct event_callback *, void *);
		void (*evcb_evfinalize)(struct event *, void *);
		void (*evcb_cbfinalize)(struct event_callback *, void *);
	} evcb_cb_union;
	void *evcb_arg;
};
/** 结构体的解释 struct event_callback {}
  ev_flags  ( = evcb_flags ) 设置周期 :
  f1.在 event_assign 
函数（大部分事件初始化都是在该函数中）中, 
     ev_flags 被初始化为    EVLIST_INIT     
     
  f2.在 event_add_internal 里调用 event_queue_insert(base,ev, 
EVLIST_INSERTED), 在函数 event_queue_insert 中
     ev_flags 被设置为      EVLIST_INIT| EVLIST_INSERTED     
     
  f3.当事件被激活时调用 event_active_nolock 函数, 
该函数内部再次调用 event_queue_insert(base, ev, EVLIST_ACTIVE);
     ev_flags 被设置为      EVLIST_INIT| EVLIST_INSERTED| 
EVLIST_ACTIVE     
     
  f4.在事件执行的时候在 event_process_active_single_queue 中, 
调用 event_queue_remove(base,ev, EVLIST_ACTIVE);
     通过 ev->ev_flags&= ~queue 语句,将取消 '激活状态'. 此时 
     ev_flags 被设置为      EVLIST_INIT| EVLIST_INSERTED
     
  f5.当端口结束工作时,会调用 event_del(struct event *ev), 
该函数调用 event_del_internal(ev),
     
在event_del_internal中会将该事件从哈希表和所有的事件队列中�
��除, 此时
     ev_flags被重新设置为   EVLIST_INIT
  综上, ev_flags 的作用主要是指出当前的 event 处于何种状态
, 则相应的函数就可以根据该状态做出处理; 
  eg. 函数 event_del_internal() 中可以判断该事件如果激活, 
则从激活队列中删除; 如果是已经插入, 
则还要从插入队列中删除.
 */


struct event_base;
/* event handler */
struct event {
    /*
     * (1) ev_evcallback 是 event 的回调函数, 被 ev_base 调用, 
执行事件处理函数.
     * (2) ev_flags 在文件 event-internal.h 是个宏定义 
     *    代码中会使用 ev->ev_flags 代替 ev->ev_evcallback.evcb_flags 
的调用.
     * (3) ev_closure 在文件 event-internal.h 是个宏定义
     *    代码中会使用 ev->ev_closure 代替 ev->ev_evcallback.
ev_closure 的调用.
     */
	struct event_callback ev_evcallback;                                        

	/* for managing timeouts, 最小堆 */
	union {
		TAILQ_ENTRY(event) ev_next_with_common_timeout;
		int min_heap_idx; /* 元素在堆中的 index */
	} ev_timeout_pos;
	evutil_socket_t ev_fd; /* 对于 I/O 事件,是绑定的文件描述符; 
对于 signal 事件,是绑定的信号 */

	short ev_events;	/* 事件类型: I/O事件, 定时事件(EV_TIMEOUT, 0x01), 
信号事件( EV_SIGNAL ), 永久事件( EV_PERSIST ) */
	short ev_res;		/* result passed to event callback, 
记录了当前激活事件的类型 */

	struct event_base *ev_base;

	union {
		/* used for io events */
		struct {
			LIST_ENTRY (event) ev_io_next; /* next event */
			struct timeval ev_timeout;     /* timeout of event */
		} ev_io;

		/* used by signal events */
		struct {
			LIST_ENTRY (event) ev_signal_next; /* doubule linked list */
			short ev_ncalls;   /* 事件就绪执行时,调用 ev_callback 的次数,
通常为1 */
			/* Allows deletes in callback */
			short *ev_pncalls; /* 指针,通常指向 ev_ncalls 或者为 NULL */
		} ev_signal;
	} ev_;

	struct timeval ev_timeout;
};

TAILQ_HEAD (event_list, event);

#ifdef EVENT_DEFINED_TQENTRY_
#undef TAILQ_ENTRY
#endif

#ifdef EVENT_DEFINED_TQHEAD_
#undef TAILQ_HEAD
#endif

LIST_HEAD (event_dlist, event); 

#ifdef EVENT_DEFINED_LISTENTRY_
#undef LIST_ENTRY
#endif

#ifdef EVENT_DEFINED_LISTHEAD_
#undef LIST_HEAD
#endif

#ifdef __cplusplus
}
#endif

#endif /* EVENT2_EVENT_STRUCT_H_INCLUDED_ */