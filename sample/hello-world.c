/*
  This example program provides a trivial server program that listens for TCP
  connections on port 9995.  When they arrive, it writes a short message to
  each client connection, and closes each connection once it is flushed.

  Where possible, it exits cleanly in response to a SIGINT (ctrl-c).
*/

/*
 * ���ͻ������ӵ���������ʱ�򣬷������򵥵���ͻ��˷���һ����Hello, World!����Ϣ��Ȼ��ȴ��ͻ��˹ر�
 * �����������ܿͻ��˷��͵�����
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

static const char MESSAGE[] = "Hello, World!\n";

static const int PORT = 9995;

// �����ص���������һ�������ӵ�����ʱ��Ļص�������
static void listener_cb(struct evconnlistener *, evutil_socket_t,
    struct sockaddr *, int socklen, void *);
	
// д�ص�����
static void conn_writecb(struct bufferevent *, void *);

// �ͻ��˹رջص�����
static void conn_eventcb(struct bufferevent *, short, void *);

//�źŴ�����
static void signal_cb(evutil_socket_t, short, void *);

int
main(int argc, char **argv)
{
	// event_base���󣨼�Reactorʵ����
	struct event_base *base;
	struct evconnlistener *listener;
	// �ź��¼�������
	struct event *signal_event;

	// 地址
	struct sockaddr_in sin;
	
	// �����windows�������ٵ���socket��غ���֮ǰ��Ҫ���ж�̬���ʼ��
#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(0x0201, &wsa_data);
#endif

	// ����һ��event_baseʵ������һ��Reactorʵ����
	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Could not initialize libevent!\n");
		return 1;
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(PORT);

	// �� 1 ��ʵ�α�ʾ : ������event_base����
	// �� 4 ��ʵ���Ǳ�־ : ��ַ�ɸ��ã�����exec��ʱ��ر��׽���
	listener = evconnlistener_new_bind(base, listener_cb, (void *)base,
	    LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1,
	    (struct sockaddr*)&sin,
	    sizeof(sin));

	if (!listener) {
		fprintf(stderr, "Could not create a listener!\n");
		return 1;
	}

	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);

	// ���¼���������ӵ�event_base���¼�������ע�������.
	if (!signal_event || event_add(signal_event, NULL)<0) {
		fprintf(stderr, "Could not create/add a signal event!\n");
		return 1;
	}

	// ����ѭ��
	event_base_dispatch(base);

    // �ͷż�����
	evconnlistener_free(listener);

	// �ͷ��¼�������
	event_free(signal_event);

	// �ͷ� event_base ����
	event_base_free(base);

	printf("done\n");
	return 0;
}

static void
listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *sa, int socklen, void *user_data)
{
	struct event_base *base = user_data;
	// ������
	struct bufferevent *bev;

    // �����׽��ִ���һ�����������׽��ֽ��յ�����֮�����ڻ�������,
    // �׽����ڷ�������֮ǰ�Ƚ����ݴ���ڻ������У�
	bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		fprintf(stderr, "Error constructing bufferevent!");
		event_base_loopbreak(base);
		return;
	}
	bufferevent_setcb(bev, NULL, conn_writecb, conn_eventcb, NULL);

	// ���û�������д����
	bufferevent_enable(bev, EV_WRITE);

	// ���û������Ķ�����
	bufferevent_disable(bev, EV_READ);

	// ������д
	bufferevent_write(bev, MESSAGE, strlen(MESSAGE));
}

static void
conn_writecb(struct bufferevent *bev, void *user_data)
{
	// ���buffer
	struct evbuffer *output = bufferevent_get_output(bev);

	// �ж������Ƿ��Ѿ��������
	if (evbuffer_get_length(output) == 0) {
		printf("flushed answer\n");
		// �ͷŻ�����
		bufferevent_free(bev);
	}
}

static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
	if (events & BEV_EVENT_EOF) {
		// �ͻ��˹ر�
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		// �������ϲ���һ������
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

	// �˳��¼�ѭ��
	event_base_loopexit(base, &delay);
}
