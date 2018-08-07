/*
  This example program provides a trivial server program that listens for TCP
  connections on port 9995.  When they arrive, it writes a short message to
  each client connection, and closes each connection once it is flushed.

  Where possible, it exits cleanly in response to a SIGINT (ctrl-c).
*/

/*
 * 当客户端链接到服务器的时候，服务器简单地向客户端发送一条“Hello, World!”消息，然后等待客户端关闭
 * 服务器不接受客户端发送的数据
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

static const char MESSAGE[] = "Hello, World! This is libevent. \r\n";

static const int PORT = 9995;

// 监听回调函数（即一个新链接到来的时候的回调函数）
static void listener_cb(struct evconnlistener *, evutil_socket_t,
    struct sockaddr *, int socklen, void *);
	
// 写回调函数
static void conn_writecb(struct bufferevent *, void *);

// 客户端关闭回调函数
static void conn_eventcb(struct bufferevent *, short, void *);

//信号处理函数
static void signal_cb(evutil_socket_t, short, void *);

int
main(int argc, char **argv)
{
	// event_base对象（即Reactor实例）
	struct event_base *base;
	struct evconnlistener *listener;
	// 信号事件处理器
	struct event *signal_event;

	// 地址
	struct sockaddr_in sin;
	
	// 如果是windows环境，再调用socket相关函数之前需要进行动态库初始化
#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(0x0201, &wsa_data);
#endif

	// 创建一个event_base实例（即一个Reactor实例）
	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Could not initialize libevent!\n");
		return 1;
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(PORT);

	// 第 1 个实参表示 : 所属的event_base对象
	// 第 4 个实参是标志 : 地址可复用，调用exec的时候关闭套接字
	listener = evconnlistener_new_bind(base, listener_cb, (void *)base,
	    LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1,
	    (struct sockaddr*)&sin,
	    sizeof(sin));

	if (!listener) {
		fprintf(stderr, "Could not create a listener!\n");
		return 1;
	}

	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);

	// 将事件处理器添加到event_base的事件处理器注册队列中.
	if (!signal_event || event_add(signal_event, NULL)<0) {
		fprintf(stderr, "Could not create/add a signal event!\n");
		return 1;
	}

	// 进入循环
	event_base_dispatch(base);

    // 释放监听器
	evconnlistener_free(listener);

	// 释放事件处理器
	event_free(signal_event);

	// 释放 event_base 对象
	event_base_free(base);

	printf("done\n");
	return 0;
}

static void
listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *sa, int socklen, void *user_data)
{
	struct event_base *base = user_data;
	// 缓冲区
	struct bufferevent *bev;

	// Test
	struct sockaddr_in sin;
	unsigned char *pcAddr;
	
    // 基于套接字创建一个缓冲区（套接字接收到数据之后存放在缓冲区中,
    // 套接字在发送数据之前先将数据存放在缓冲区中）
	bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		fprintf(stderr, "Error constructing bufferevent!");
		event_base_loopbreak(base);
		return;
	}
	bufferevent_setcb(bev, NULL, conn_writecb, conn_eventcb, NULL);

	// 启用缓存区的写功能
	bufferevent_enable(bev, EV_WRITE);

	// 禁用缓冲区的读功能
	bufferevent_disable(bev, EV_READ);
	
	// test by tian
	memcpy(&sin, sa, sizeof(struct sockaddr_in));
	pcAddr = (unsigned char *)&(sin.sin_addr.s_addr);
	
	printf("Write begin... \r\n");
	printf("The client ip is : %u.%u.%u.%u.  port is : %d \r\n", 
		pcAddr[0], pcAddr[1], pcAddr[2], pcAddr[3], sin.sin_port);

	// 缓冲区写
	bufferevent_write(bev, MESSAGE, strlen(MESSAGE));
}

static void
conn_writecb(struct bufferevent *bev, void *user_data)
{
	// 输出buffer
	struct evbuffer *output = bufferevent_get_output(bev);

	// 判断数据是否已经发送完成
	if (evbuffer_get_length(output) == 0) {
		printf("flushed answer\n");
		// 释放缓冲区
		bufferevent_free(bev);
	}
}

static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
	printf("In conn_eventcb()  \r\n");
	if (events & BEV_EVENT_EOF) {
		// 客户端关闭
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		// 在连接上产生一个错误
		printf("Got an error on the connection: %s\n",
		    strerror(errno));/*XXX win32*/
	}
	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
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
