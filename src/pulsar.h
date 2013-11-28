#ifndef __PULSAR_H__
#define __PULSAR_H__

#include <error.h>
#include <assert.h>
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
#include <uv.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef DMALLOC
#include <dmalloc.h>
#endif

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
	uv_loop_t *loop;
} pulsar_loop;

/**************************************************************************************
 ** Idlers
 **************************************************************************************/
typedef struct
{
	uv_idle_t *w_timeout;
	
	pulsar_loop *loop;

	lua_State *L;

	bool active;
	bool first_run;
	int co_ref;
} pulsar_idle;

struct pulsar_idle_worker_chain
{
	lua_State *L;
	int L_ref;
	int nargs;
	struct pulsar_idle_worker_chain *next;
};
typedef struct pulsar_idle_worker_chain pulsar_idle_worker_chain;

typedef struct
{
	uv_idle_t *w_timeout;
	
	pulsar_loop *loop;

	bool active;
	pulsar_idle_worker_chain *chain;
} pulsar_idle_worker;

/**************************************************************************************
 ** Timers
 **************************************************************************************/
typedef struct
{
	uv_timer_t *w_timeout;
	
	pulsar_loop *loop;

	lua_State *L;

	bool active;
	bool first_run;
	int co_ref;

	int timeout, repeat;
} pulsar_timer;

/**************************************************************************************
 ** TCP
 **************************************************************************************/
typedef struct
{
	uv_tcp_t *sock;
	
	pulsar_loop *loop;

	lua_State *L;

	bool active;
	int client_fct_ref;
} pulsar_tcp_server;

typedef struct
{
	uv_tcp_t *sock;

	pulsar_loop *loop;

	bool standalone;

	int co_ref;

	bool closed;
	bool active;
	bool disconnected;

	size_t read_wait_len;
	char *read_wait_until;
	size_t read_wait_ignorelen;
	char *read_wait_ignore;

	lua_State *rL;
	int rL_ref;
	char *read_buf;
	size_t read_buf_len, read_buf_pos;
} pulsar_tcp_client;

typedef struct
{
	uv_write_t req;

	uv_buf_t buf;

	pulsar_tcp_client *client;

	lua_State *sL;
	int sL_ref;
	int data_ref;

	bool nowait;
} pulsar_tcp_client_send_chain;

typedef struct
{
	uv_connect_t req;

	pulsar_loop *loop;

	uv_tcp_t *sock;

	lua_State *L;
	int L_ref;
} pulsar_tcp_client_connect;

#endif
