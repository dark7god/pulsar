#ifndef __PULSAR_H__
#define __PULSAR_H__

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>
#include <eio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define MT_PULSAR_LOOP		"Pulsar Loop"
#define MT_PULSAR_TIMER		"Pulsar Timer"
#define MT_PULSAR_IDLE		"Pulsar Idle"
#define MT_PULSAR_IDLE_WORKER	"Pulsar Idle Worker"
#define MT_PULSAR_TCP_SERVER	"Pulsar TCP Server"
#define MT_PULSAR_TCP_CLIENT	"Pulsar TCP Client"

/**************************************************************************************
 ** Loop
 **************************************************************************************/
typedef struct
{
	struct ev_loop *loop;
} pulsar_loop;

/**************************************************************************************
 ** Idlers
 **************************************************************************************/
typedef struct
{
	struct ev_idle w_timeout;
	
	pulsar_loop *loop;

	lua_State *L;

	bool active;
	bool first_run;
	int co_ref;
} pulsar_idle;

struct pulsar_idle_worker_chain
{
	lua_State *L;
	struct pulsar_idle_worker_chain *next;
};
typedef struct pulsar_idle_worker_chain pulsar_idle_worker_chain;

typedef struct
{
	struct ev_idle w_timeout;
	
	pulsar_loop *loop;

	bool active;
	pulsar_idle_worker_chain *chain;
} pulsar_idle_worker;

/**************************************************************************************
 ** Timers
 **************************************************************************************/
typedef struct
{
	struct ev_timer w_timeout;
	
	pulsar_loop *loop;

	lua_State *L;

	bool active;
	bool first_run;
	int co_ref;
} pulsar_timer;

/**************************************************************************************
 ** TCP
 **************************************************************************************/
typedef struct
{
	struct ev_io w_accept;
	
	pulsar_loop *loop;

	lua_State *L;

	int fd;

	bool active;
	int client_fct_ref;
} pulsar_tcp_server;

struct pulsar_tcp_client_send_chain
{
	lua_State *sL;
	bool send_buf_nowait;
	char *send_buf;
	size_t send_buf_len;

	struct pulsar_tcp_client_send_chain *next;
};
typedef struct pulsar_tcp_client_send_chain pulsar_tcp_client_send_chain;

typedef struct
{
	struct ev_io w_read;
	struct ev_io w_send;

	pulsar_loop *loop;

	bool standalone;
	int fd;

	int co_ref;

	bool active;
	bool disconnected;

	size_t read_wait_len;
	char *read_wait_until;
	size_t read_wait_ignorelen;
	char *read_wait_ignore;

	lua_State *rL;
	char *read_buf;
	size_t read_buf_len, read_buf_pos;

	lua_State *sL;
	bool send_buf_nowait;
	char *send_buf;
	size_t send_buf_len, send_buf_pos;
	pulsar_tcp_client_send_chain *send_buf_chain;
} pulsar_tcp_client;

extern void pulsar_eio_init(struct ev_loop *loop);
extern int pulsar_resolve_dns(lua_State *L);

#endif
