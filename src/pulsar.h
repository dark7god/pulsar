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
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

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

	int fd;

	int co_ref;

	bool active;
	bool disconnected;

	size_t read_wait_len;
	char *read_wait_until;

	lua_State *rL;
	char *read_buf;
	size_t read_buf_len, read_buf_pos;

	lua_State *sL;
	bool send_buf_nowait;
	char *send_buf;
	size_t send_buf_len, send_buf_pos;
	pulsar_tcp_client_send_chain *send_buf_chain;
} pulsar_tcp_client;

#endif
