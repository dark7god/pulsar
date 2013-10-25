#include "pulsar.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

typedef struct {
	lua_State *L;
	char *host;
	struct addrinfo *address;
} resolve_t;

static ev_idle idle_watcher;
static ev_async async_watcher;
static struct ev_loop *eio_loop = NULL;

// This is only executed when eio_poll returns -1
static void idle_cb(struct ev_loop *loop, ev_idle *w, int revents) {
	if (eio_poll() != -1) ev_idle_stop(loop, w);
}

static void async_cb(struct ev_loop *loop, ev_async *w, int revents) {
	if (eio_poll() == -1) {
		ev_idle_start(loop, &idle_watcher);
	}
}

// Need to call eio_poll, but not from this function.
// Signal async_watcher instead and call from there.
static void want_poll(void) {
	ev_async_send(eio_loop, &async_watcher);
}

static int on_resolve(eio_req *req) {
	char ipstr[INET6_ADDRSTRLEN];
	resolve_t *r = (resolve_t *)req->data;
	struct addrinfo *p;
	
	for (p = r->address; p; p = p->ai_next) {
		void *addr;

		if (p->ai_family == AF_INET) { // IPv4
			struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
			addr = &ipv4->sin_addr;
		} else { // IPv6
//			struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
//			addr = &ipv6->sin6_addr;
			continue;
		}

		inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
		lua_pushstring(r->L, ipstr);
		lua_resume(r->L, 1);
		break;
	}
	
	free(r->host);
	freeaddrinfo(r->address);
	free(r);
	return 0;
}

static void resolve(eio_req *req) {
	resolve_t *r = (resolve_t *)req->data;
	struct addrinfo hints;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	req->result = getaddrinfo(r->host, "80", &hints, &r->address);
}

int pulsar_resolve_dns(lua_State *L) {
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	const char *host = luaL_checkstring(L, 2);

	resolve_t *r = malloc(sizeof(resolve_t));
	r->host = strdup(host);
	r->L = L;
	eio_custom(resolve, 0, on_resolve, r);
	return lua_yield(L, 0);
}

void pulsar_eio_init(struct ev_loop *loop) {
	eio_loop = loop;
	ev_idle_init(&idle_watcher, idle_cb);
	ev_async_init(&async_watcher, async_cb);
	eio_init(want_poll, 0);
	ev_async_start(loop, &async_watcher);
}
