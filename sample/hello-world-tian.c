/*
  This example program provides a trivial server program that listens for TCP
  connections on port 9995.  When they arrive, it writes a short message to
  each client connection, and closes each connection once it is flushed.

  Where possible, it exits cleanly in response to a SIGINT (ctrl-c).
*/

/*
 * 1. 主线程主要是监听用户的socket连接事件；工作线程主要监听socket的读写事件
 *
 */

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#ifndef _WIN32
#include <netinet/in.h>
# ifdef _XOPEN_SOURCE_EXTENDED
#  include <arpa/inet.h>
# endif
#include <sys/socket.h>
#endif

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include "event2/thread.h"
#include "pthread.h"

#include <sys/time.h>

/** CQ_ITEM queue */
#include <stdlib.h>

static const char MESSAGE[] = "Hello, world! This is libevent. \r\n";

static const int PORT = 9995;

// 线程pool中的子线程总线
#define THREAD_NUM 5
// 线程均衡调度时记录上一次用过的线程 id
int nLastThreadTid = 0;

#define BUF_SIZE 1024

typedef struct {
	pthread_t tid;
	struct event_base *base;
}TDispatcherThread;
TDispatcherThread stDispatcherThread;

/** 
 * Possible states of a connection.  enum conn_states{} - Copy from memcached-1.5.10 
 */
enum conn_states {
    conn_listening,  /**< the socket which listens for connections */
    conn_new_cmd,    /**< Prepare connection for next command */
    conn_waiting,    /**< waiting for a readable socket */
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      /**< writing out a simple response */
    conn_nread,      /**< reading in a fixed number of bytes */
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    /**< closing this connection */
    conn_mwrite,     /**< writing out many items sequentially */
    conn_closed,     /**< connection is closed */
    conn_watch,      /**< held by the logger thread as a watcher */
    conn_max_state   /**< Max state value (used for assertion) */
};
	
/** 
 * enum network_transport{} - Copy from memcached-1.5.10 
 */
enum network_transport {
    local_transport, /* Unix sockets*/
    tcp_transport,
    udp_transport
};

/* An item in the connection queue. */
enum conn_queue_item_modes {
    queue_new_conn,   /* brand new connection. */
    queue_redispatch, /* redispatching from side thread */
};

/* An item in the connection queue. */
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
    int               sfd;   		//socket的fd
    enum conn_states  init_state; 	//事件类型
    int               event_flags; 	//libevent的flags
    int               read_buffer_size; //读取的buffer的size
    enum network_transport     transport; 	
    enum conn_queue_item_modes mode;
    CQ_ITEM          *next; 		//下一个item的地址
};

/* A connection queue. */
typedef struct conn_queue {
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
}CQ;

// 需要保存的信息结构，用于管道通信和基事件的管理
typedef struct {
	pthread_t tid;
	struct event_base *base;
	struct event *notify_event;	/** listen event for notify pipe */
	int read_fd;				/** 管道接收端    */
	int write_fd;				/** 管道发送端    */
	int connect_fd;
	char *buffer;
	// 新连接的队列结构
	struct conn_queue *new_conn_queue;
}TLibeventThread;
//TLibeventThread *pLibeventThread = (TLibeventThread *)calloc(THREAD_NUM, sizeof(TLibeventThread));
TLibeventThread pLibeventThread[THREAD_NUM];

// 监听回调函数（即一个新链接到来的时候的回调函数）
static void listener_cb(struct evconnlistener *, evutil_socket_t, struct sockaddr *, int socklen, void *);
// 主 Loop 超时回调函数
static void timeout_cb(evutil_socket_t fd, short event, void *arg);
// Child thread proces func.
static void thread_libevent_process_cb(evutil_socket_t fd, short event, void *arg);
// 写回调函数
static void conn_writecb(evutil_socket_t fd, short event, void *arg);
// 客户端关闭回调函数
static void conn_eventcb(struct bufferevent *, short, void *);
//信号处理函数
static void signal_cb(evutil_socket_t, short, void *);
// 读回调函数
static void conn_readcb(evutil_socket_t fd, short event, void *arg);
// Init Child Thread(read & write data).
static int create_pthread_pool(void);
// Child Thread func(read & write data).
static void * work_thread(void *arg);

/** CQ_ITEM queue */
#define ITEMS_PER_ALLOC 64
/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;
/** CQ_ITEM  queue 的操作接口函数 */
static CQ_ITEM *cqi_new(void);
static CQ_ITEM *cq_pop(CQ *cq);

/** The structure representing a connection into memcached. */
struct conn {	
    int    sfd;
	struct conn   	*next;	  /* Used for generating a list of conn structures */
	TLibeventThread *thread; /* Pointer to the thread object serving this connection */
};

/** variables */
static int max_fds;
struct conn **conns;

static void conn_init(void);
struct conn *conn_new(const int sfd, const enum conn_states init_state, const int event_flags, const int read_buffer_size, enum network_transport transport, struct event_base *base);

int
main(int argc, char **argv)
{	
	int i = 0;			// for 循环的迭代次数			
	int nRet = 0; 		// return Value, if !=0 means error.	
	int fd[2] = {0};	// read, write 描述符
	// event_base对象（即Reactor实例）
	struct event_base *base;
	struct evconnlistener *listener;
	// 信号事件处理器,      主loop 超时timer, 
	struct event *signal_event;
	struct event *timeout;
	// 超时 timer 定时值
	struct timeval tv;

	// 地址
	struct sockaddr_in sin;
	
	// 如果是windows环境，再调用socket相关函数之前需要进行动态库初始化
#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(0x0201, &wsa_data);
#endif

	/** Use Muti-thread */
	evthread_use_pthreads();
	stDispatcherThread.tid = pthread_self();	

	// 创建一个event_base实例（即一个Reactor实例）
	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Could not initialize libevent!\n");
		return 1;
	}
	/** Clear 0, and Init  */
	memset(&stDispatcherThread, 0, sizeof(TDispatcherThread));
	stDispatcherThread.base = base;

    /** initialize other stuff */
	conn_init();

	/** Init Child thread. */
	for(i=0; i<THREAD_NUM; i++)
	{
		nRet = evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
		if(-1 == nRet)
		{
			printf("evutil_socketpair() error.  \r\n");
			return nRet;
		}
		pLibeventThread[i].read_fd  = fd[1];		
		pLibeventThread[i].write_fd = fd[0];
		pLibeventThread[i].base		= event_base_new();
		if (NULL == pLibeventThread[i].base) 
		{
			printf("Could not initialize libevent for pLibeventThread. \r\n");
			return -1;
		}

		pLibeventThread[i].notify_event = event_new(pLibeventThread[i].base, pLibeventThread[i].read_fd, EV_READ| EV_PERSIST, 
			thread_libevent_process_cb, &pLibeventThread[i]);

		// 将事件处理器添加到event_base的事件处理器注册队列中.		
		if(!(pLibeventThread[i].notify_event) || event_add(pLibeventThread[i].notify_event, 0)<-1)
		{
			printf("event_add() error : pLibeventThread[i].ev.\r\n");
			return -1;
		}		
	}

	if(create_pthread_pool() < 0)
	{
		printf("create_pthread_pool() error. \r\n");
		return -1;
	}

	/** 
	  * Mian-Loop timer 
	  * 1. EV_PERSIST 可以用于标识一个 持续 的超时事件,  而 evtimer_new() 只是用于单次触发的事件, 则需要
	  *   每次在 timeout_cb() 中重新设置 & ADD 超时 timer 值
	  * 2. 2018-08-15 event_new() 中可以设置 EV_PERSIST 标识则用于 周期触发 timeout_cb, 且
	  *   无需在 timeout_cb ADD tiemr 值
	  */	
	timeout = event_new(stDispatcherThread.base, -1, EV_PERSIST, timeout_cb, timeout);
	//timeout = evtimer_new(stDispatcherThread.base, timeout_cb, timeout);
	//evutil_timerclear(&tv);	// 暂时屏蔽否则找不到 <sys/time.h> 文件中的函数声明
	tv.tv_sec = tv.tv_usec = 0;
	tv.tv_sec = 5;	
	//  timeout处 理器添加到event_base的事件处理器注册队列中.
	if (!timeout || event_add(timeout, &tv)<0) {
		printf("event_add() error : timeout.\r\n");
		return -1;
	}
	
	/** before using sockaddr_in,  need Clear  it */
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(PORT);

	// 第 1 个实参表示 : 所属的event_base对象; 第 4 个实参是标志 : 地址可复用，调用exec的时候关闭套接字
	// s1. 注册当发生某一操作(比如接受来自客户端的连接)时应该执行的函数.
	listener = evconnlistener_new_bind(stDispatcherThread.base, listener_cb, (void *)stDispatcherThread.base,
	    LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1,
	    (struct sockaddr*)&sin,
	    sizeof(sin));

	if (!listener) {
		fprintf(stderr, "Could not create a listener!\n");
		return 1;
	}

	/** 2018-08-14  signal_cb , there is no use in code , and reserved. */
	signal_event = evsignal_new(stDispatcherThread.base, SIGINT, signal_cb, (void *)stDispatcherThread.base);

	// 将事件处理器添加到event_base的事件处理器注册队列中.
	if (!signal_event || event_add(signal_event, NULL)<0) {
		fprintf(stderr, "Could not create/add a signal event!\n");
		return -1;
	}

	// s2. 主事件循环, 进入循环.
	event_base_dispatch(stDispatcherThread.base);

    // s.Final.  释放监听器, 一般情况, 只要 event_base_loop 在循环中, 则不会执行到下面的函数; 除非程序结束
	evconnlistener_free(listener);

	// 释放事件处理器
	event_free(signal_event);
	event_free(timeout);

	// 释放 event_base 对象
	event_base_free(stDispatcherThread.base);
	for(i=0; i<THREAD_NUM; i++){
		event_base_free(pLibeventThread[i].base);
	}

	printf("done\n");
	return 0;
}


/**
 * listener_cb(...)  
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
static void
listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *sa, int socklen, void *user_data)
{
	struct event_base *base = user_data;
	// 缓冲区
	struct bufferevent *bev;

	// Test : print Client IP & port
	struct sockaddr_in sin;
	unsigned char *pcAddr;
	
    //每个连接连上来的时候, 都会申请一块CQ_ITEM的内存块, 用于存储连接的基本信息
    //CQ_ITEM *pItem = cqi_new();

	/** 线程分发 */		
	/** memcached中线程负载均衡算法 - 有待分析 by tian           */
	int nTid = (nLastThreadTid + 1) % THREAD_NUM;
	TLibeventThread *thread = pLibeventThread + nTid;
	nLastThreadTid = nTid;

	thread->connect_fd = fd;

	//pItem->sfd = fd;
	//pItem->init_state = conn_new_cmd;
	//pItem->event_flags = EV_READ|EV_PERSIST;
	//pItem->read_buffer_size = 1024; 	// @2018-08-17 ! ! ! 临时写为 1024
	//pItem->transport = tcp_transport;
	//pItem->mode = queue_new_conn;
	//向工作线程的队列中放入CQ_ITEM

	// 线程读写 child-thread,  先 write 1 个无实际意义字节c, 可以触发该 child-thread 的 thread_libevent_process_cb()
	write(thread->write_fd, "c", 1);	
	
#if 0	
    // 1. 基于套接字创建一个缓冲区（套接字接收到数据之后存放在缓冲区中,套接字在发送数据之前先将数据存放在缓冲区中）
    // 2. 文件描述符 fd 对应最新的 connected 到服务器上的 socket fd.
	bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_THREADSAFE);
	if (!bev) {
		fprintf(stderr, "Error constructing bufferevent!");
		event_base_loopbreak(base);
		return;
	}

	/** Set timeout */
	struct timeval tv_read = {10, 0}, tv_write = {10, 0};
	bufferevent_set_timeouts(bev, &tv_read, &tv_write);
	// Enbale Muti-thread
	if(bufferevent_enable(bev, BEV_OPT_THREADSAFE) < 0)
	{
		printf("bufferevent_enable : BEV_OPT_THREADSAFE, failed. \r\n");
	}	

	/** use user_data for on-shared communicate , and when to be free ?  */
	char *userData = NULL;
	bufferevent_setcb(bev, conn_readcb, conn_writecb, conn_eventcb, userData);

	// 禁用缓存区的写功能
	//bufferevent_disable(bev, EV_WRITE);
	//bufferevent_enable(bev, EV_WRITE);

	// 启用缓冲区的读      & 写功能
	bufferevent_enable(bev, EV_READ);
	bufferevent_enable(bev, EV_WRITE);

	// 缓冲区写
	//bufferevent_write(bev, MESSAGE, strlen(MESSAGE));
#endif //#if 0

	// test by tian , print the ADDR of Client.
	memcpy(&sin, sa, sizeof(struct sockaddr_in));
	pcAddr = (unsigned char *)&(sin.sin_addr.s_addr);	
	printf("The client ip is : %u.%u.%u.%u.  port is : %d \r\n", 
		pcAddr[0], pcAddr[1], pcAddr[2], pcAddr[3], sin.sin_port);
}

static void
conn_writecb(evutil_socket_t fd, short event, void *arg)
{
	//printf("conn_writecb() : enter \r\n");
	TLibeventThread *ev_thread = (TLibeventThread*)arg;

	if(NULL == arg){
		printf("conn_writecb() :  NULL == arg \r\n");
		return;
	}

	write(fd, ev_thread->buffer, 128);
	ev_thread->buffer = NULL;
		
	//printf("conn_writecb() : ok \r\n");
}

static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
	printf("conn_eventcb() : enter\r\n");
	if (events & BEV_EVENT_EOF) {
		// 客户端关闭
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		// 在连接上产生一个错误
		printf("Got an error on the connection: %s\n",
		    strerror(errno));/*XXX win32*/
	} else if(events & BEV_EVENT_TIMEOUT)	{
		// timeout : reading or writing
		printf("User-specified timeout reached. \r\n");
	}

	bufferevent_free(bev);
	printf("conn_eventcb() : ok \r\n");
}

static void
signal_cb(evutil_socket_t sig, short events, void *user_data)
{
	struct event_base *base = user_data;
	struct timeval delay = { 2, 0 };

	printf("Caught an interrupt signal; exiting cleanly in two seconds.\n");

	// 退出事件循环
	event_base_loopexit(base, &delay);
}

static void
conn_readcb(evutil_socket_t fd, short event, void *arg)
{
	//printf("conn_readcb() : enter \r\n");

	char cBuf[128] = {0};
	int len = 0;	
	struct event *evSend;

	if(NULL == arg){
		printf("conn_readcb() :  NULL == arg \r\n");
		return;
	}

	TLibeventThread *ev_thread =(TLibeventThread*)arg;
	len = read(fd, cBuf, 128);
	if(0 == len){
		// means the socket fd has been Closed.
		printf("socket %d has been Closed. \r\n", fd);
		close(fd);
		return;
	}

	ev_thread->buffer = cBuf;
	printf("Thread %llu has read : %s \r\n", ev_thread->tid, ev_thread->buffer);
	
	evSend = event_new(ev_thread->base, ev_thread->connect_fd, EV_WRITE|EV_PERSIST, conn_writecb, ev_thread);
	// 将事件处理器添加到 这个 thread -base  事件处理器注册队列中.	
	if(!evSend || event_add(evSend, 0)<-1)
	{
		printf("event_add() error : evSend \r\n");
		return;
	}	

	//printf("conn_readcb() : ok \r\n");	
	return;
}

static int create_pthread_pool(void)
{
	int nRet = 0 , i = 0;
	for(i=0; i<THREAD_NUM; i++)
	{
		nRet = pthread_create(&pLibeventThread[i].tid, NULL, work_thread, &pLibeventThread[i]);
		if(0 != nRet)
		{
			printf("create_pthread_pool %d error. \r\n", nRet);
			return -1;
		}
	}
	printf("create_pthread_pool : ok \r\n");
	return 1;
}

static void * work_thread(void *arg)
{
	printf("work_thread() : enter \r\n");
	TLibeventThread *self = (TLibeventThread *)arg;
	self->tid = pthread_self();

	// 每个工作线程都在检测event链表是否有事件发生
	event_base_dispatch(self->base);

	// 2018-08-15 On normally, Code will not run at the following line, otherwise the Thread error.
	printf("work_thread() : ok \r\n");
	return NULL;
}

static void timeout_cb(evutil_socket_t fd, short event, void *arg)
{
	printf("timeout_cb() : enter \r\n");
#if 0	
	struct event *timeout = (struct event *)arg;

	/** memcached中线程负载均衡算法 - 有待分析 by tian           */
	int nTid = (nLastThreadTid + 1) % THREAD_NUM;
	TLibeventThread *thread = pLibeventThread + nTid;
	nLastThreadTid = nTid;

	/**
	  * 线程读写
	  * 1. child-thread used for "Parser Data"
	  */
	write(thread->write_fd, "Hello world. It is timeout_cb()", sizeof("Hello world. It is timeout_cb()") - 1);	
#endif //#if 0
	printf("timeout_cb() : ok \r\n");
}

static void thread_libevent_process_cb(evutil_socket_t fd, short event, void *arg)
{
	printf("thread_libevent_process_cb() : enter \r\n");

	TLibeventThread *self = (TLibeventThread *)arg;	
	CQ_ITEM *pItem;	
	char cBuf[1] = {0};	
    struct conn *c;
	int nRecv = 0;
	struct event *evRecv;	
    unsigned int timeout_fd;

	if(fd != self->read_fd)
	{
		printf("thread_libevent_process_cb error : fd != self->read_fd \r\n");
		return;
	}

	nRecv = read(fd, cBuf, 1);
	if(nRecv != 1)
	{
		return;
	}	
	//cBuf[nRecv] = '\0';

	switch (cBuf[0]){
		case 'c':{			
			printf("thread %llu receive message : %c \r\n", (pthread_t)self->tid, cBuf[0]);
			//pItem = cq_pop(self->new_conn_queue); 	// !!!  2018-08-17   需要解决的代码  
			if(NULL == pItem) 
			{
				break;
			}
			
			switch (pItem->mode){
				case queue_new_conn:{
					//c = conn_new();
					break;
				}
				case queue_redispatch:{
					//conn_worker_readd(pItem->c);
					break;
				}
			}
			//cqi_free();
			break;
		}		
		/* a client socket timed out */
		case 't':{
			// Read	 ---  timeout_fd
			break;
		}			
	}

	evRecv = event_new(self->base, self->connect_fd, EV_READ|EV_PERSIST, conn_readcb, self);	
	// 将事件处理器添加到 这个 thread -base  事件处理器注册队列中.	
	if(!evRecv || event_add(evRecv, 0)<-1)
	{
		printf("event_add() error : evRecv \r\n");
		return;
	}

	printf("thread_libevent_process_cb() : ok \r\n");
	return;	
}

/*
 * Returns a fresh connection queue item.
 */
static CQ_ITEM *cqi_new(void) {
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist) {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item) {
        int i;

        /* Allocate a bunch of items at once to reduce fragmentation */
        item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item) {
            //STATS_LOCK();
            //stats.malloc_fails++;
            //STATS_UNLOCK();
            return NULL;
        }

        /*
         * Link together all the new items except the first one
         * (which we'll return to the caller) for placement on
         * the freelist.
         */
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}

/*
 * Looks for an item on a connection queue, but doesn't block if there isn't
 * one.
 * Returns the item, or NULL if no item is available
 */
static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/*
 * Initializes the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) 
{
	max_fds = 0xFF;
	
}

struct conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base) {
    //struct conn *c;
}



