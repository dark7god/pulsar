/*
 ***** Pulsar
 * A libev to lua coroutine bind
 *
 * Copyright Nicolas Casalini 2013
 */

#include "pulsar.h"

#if !defined LUA_VERSION_NUM || LUA_VERSION_NUM==501
/*
** Adapted from Lua 5.2.0
*/
static void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup) {
  luaL_checkstack(L, nup, "too many upvalues");
  for (; l->name != NULL; l++) {  /* fill the table with given functions */
    int i;
    for (i = 0; i < nup; i++)  /* copy upvalues to the top */
      lua_pushvalue(L, -nup);
    lua_pushstring(L, l->name);
    lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
    lua_settable(L, -(nup + 3));
  }
  lua_pop(L, nup);  /* remove upvalues */
}
#endif

#define DEFAULT_BUFFER_SIZE	1024
#define WAIT_LEN_UNTIL		-1

/*
** Define the metatable for the object on top of the stack
*/
static void pulsar_setmeta (lua_State *L, const char *name) {
	luaL_getmetatable (L, name);
	lua_setmetatable (L, -2);
}

static int traceback(lua_State *L) {
	lua_Debug ar;
	int n = 0;
	while(lua_getstack(L, n++, &ar)) {
		lua_getinfo(L, "nSl", &ar);
		printf("\tAt %s:%d %s\n", ar.short_src, ar.currentline, ar.name?ar.name:"");
	}
	return 1;
}

static void stackDump (lua_State *L) {
	int i=lua_gettop(L);
	printf(" ----------------  Stack Dump ----------------\n" );
	while(  i   ) {
		int t = lua_type(L, i);
		switch (t) {
		case LUA_TSTRING:
			printf("%d:`%s'\n", i, lua_tostring(L, i));
			break;
		case LUA_TBOOLEAN:
			printf("%d: %s\n",i,lua_toboolean(L, i) ? "true" : "false");
			break;
		case LUA_TNUMBER:
			printf("%d: %g\n",  i, lua_tonumber(L, i));
			break;
		default:
			printf("%d: %s // %x\n", i, lua_typename(L, t), (int)lua_topointer(L, i));
			break;
		}
		i--;
	}
	printf("--------------- Stack Dump Finished ---------------\n" );
}


static int pulsar_panic(lua_State *L) {
	printf("OMFG PANIC: %s\n", lua_tostring(L, -1));
	stackDump(L);
	traceback(L);
	return 0;
}
static int pulsar_panic_main(lua_State *L) {
	printf("OMFG PANIC MAIN: %s\n", lua_tostring(L, -1));
	stackDump(L);
	traceback(L);
	return 0;
}

static void buf_alloc(uv_handle_t* tcp, size_t size, uv_buf_t *b) {
	b->len = DEFAULT_BUFFER_SIZE;
	b->base = (char*)malloc(sizeof(char) * DEFAULT_BUFFER_SIZE);
}

static void buf_free(uv_buf_t *b) {
	free(b->base);
}

/**************************************************************************************
 ** TCP Client calls
 **************************************************************************************/
static void pulsar_client_resume(pulsar_tcp_client *client, lua_State *L, int nargs);

static void close_cb(uv_handle_t* handle) {
	free((void*)handle);
}

static void client_close(pulsar_tcp_client *client) {
	if (client->closed) return;
	client->closed = true;

	// Resume waiting coroutines so that they can fail
	if (client->read_wait_len) {
		client->read_wait_len = 0;
		if (lua_status(client->rL) == LUA_YIELD) {
			lua_pushnil(client->rL);
			lua_pushliteral(client->rL, "disconnected");
			pulsar_client_resume(client, client->rL, 2);
			luaL_unref(client->rL, LUA_REGISTRYINDEX, client->rL_ref);
		}
	}

	if (client->active) uv_read_stop((uv_stream_t*)client->sock);
	client->active = false;
	client->disconnected = true;
	uv_close((uv_handle_t*)client->sock, close_cb);

	if (client->read_buf) free(client->read_buf);
	client->read_buf = NULL;
}

static void pulsar_client_resume(pulsar_tcp_client *client, lua_State *L, int nargs) {
	if (client->closed) return;
	int ret = lua_resume(L, nargs);
	// More to do
	if (!ret) return;
	if (ret == LUA_YIELD) return;
	if (!client) return;
	if (client->standalone) return;

	// Finished, end the client
/*	if (!ret) {
		printf("Closing at resume %lx\n", client);
		client_close(client);
		// Let the rest free up by GC
		return;
	}
*/
	if (ret == LUA_ERRRUN) {
		printf("Error while running client's coroutine (fd): %s\n", lua_tostring(L, -1));
		stackDump(L);
		traceback(L);
		printf("Closing at error %lx\n", client);
		client_close(client);
		// Let the rest free up by GC
		return;
	}
}

static int pulsar_tcp_client_close(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	client_close(client);
	return 0;
}

static int pulsar_tcp_client_is_connected(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	lua_pushboolean(L, !client->closed && client->active && !client->disconnected);
	return 1;
}
static int pulsar_tcp_client_has_data(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	lua_pushboolean(L, !client->closed && client->read_buf_pos > 0);
	return 1;
}

static void tcp_client_send_cb(uv_write_t *_req, int status){
	pulsar_tcp_client_send_chain *req = (pulsar_tcp_client_send_chain*)_req;
	pulsar_tcp_client *client = req->client;

	//free(req->buf.base);
	if (!req->nowait) {
		lua_pushnumber(req->sL, req->buf.len);
		pulsar_client_resume(client, req->sL, 1);
	}
	luaL_unref(req->sL, LUA_REGISTRYINDEX, req->data_ref);
	luaL_unref(req->sL, LUA_REGISTRYINDEX, req->sL_ref);

	free(req);
}

static int pulsar_tcp_client_send(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (client->closed) return 0;
	size_t datalen;
	const char *data = lua_tolstring(L, 2, &datalen);
	bool nowait = lua_toboolean(L, 3);

	pulsar_tcp_client_send_chain *req = (pulsar_tcp_client_send_chain*)malloc(sizeof(pulsar_tcp_client_send_chain));
	req->client = client;
	req->buf.len = datalen;
	req->buf.base = (char*)data;

	uv_write((uv_write_t*)req, (uv_stream_t*)client->sock, &req->buf, 1, tcp_client_send_cb);

	lua_pushthread(L); req->sL = lua_tothread(L, -1); req->sL_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	req->nowait = nowait;

	lua_pushvalue(L, 2);
	req->data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	
	if (nowait) return 0;
	else return lua_yield(L, 0);
	return 0;
}

static void tcp_client_read_cb(uv_stream_t *watcher, ssize_t read, const uv_buf_t *buf){
	pulsar_tcp_client *client = (pulsar_tcp_client *)watcher->data;
	if (client->closed) return;

	if (read < 0)
	{
		client_close(client);
		return;
	}
	if (read == 0) return;
	
	if (client->read_buf_len - client->read_buf_pos < read) {
		char *newbuf = malloc(client->read_buf_len + read);
		memcpy(newbuf, client->read_buf, client->read_buf_pos);
		free(client->read_buf);
		client->read_buf = newbuf;
		client->read_buf_len += read;
	}

	memcpy(client->read_buf + client->read_buf_pos, buf->base, read);
	buf_free((uv_buf_t*)buf);

	client->read_buf_pos += read;

	if ((client->read_buf_pos) && (client->read_wait_len > 0) && (client->read_buf_pos >= client->read_wait_len)) {
		lua_pushlstring(client->rL, client->read_buf, client->read_wait_len);
		char *newbuf = malloc(client->read_buf_len);
		if (client->read_buf_pos > client->read_wait_len) memcpy(newbuf, client->read_buf + client->read_wait_len, client->read_buf_pos - client->read_wait_len);
		free(client->read_buf);
		client->read_buf = newbuf;
		client->read_buf_pos -= client->read_wait_len;
		client->read_wait_len = 0;

		pulsar_client_resume(client, client->rL, 1);
		luaL_unref(client->rL, LUA_REGISTRYINDEX, client->rL_ref);
		return;
	}

	if ((client->read_buf_pos) && (client->read_wait_len == WAIT_LEN_UNTIL)) {
		size_t len = strlen(client->read_wait_until);
		const char *until = client->read_wait_until;
		size_t ignorelen = client->read_wait_ignorelen;
		const char *ignore = client->read_wait_ignore;
		size_t pos = 0;
		while (pos <= client->read_buf_pos - len) {
			if (!memcmp(client->read_buf + pos, until, len)) {
				if (ignorelen && (ignorelen <= pos) && !memcmp(client->read_buf + pos - ignorelen, ignore, ignorelen))
					lua_pushlstring(client->rL, client->read_buf, pos - ignorelen);
				else {
					lua_pushlstring(client->rL, client->read_buf, pos);
				}

				char *newbuf = malloc(client->read_buf_len);
				if (client->read_buf_pos > pos + len) memcpy(newbuf, client->read_buf + pos + len, client->read_buf_pos - pos - len);
				free(client->read_buf);
				client->read_buf = newbuf;
				client->read_buf_pos -= pos + len;
				client->read_wait_len = 0;
				free(client->read_wait_until);
				if (client->read_wait_ignorelen) free(client->read_wait_ignore);
				client->read_wait_ignore = NULL;
				client->read_wait_ignorelen = 0;
				client->read_wait_until = NULL;

				pulsar_client_resume(client, client->rL, 1);
				luaL_unref(client->rL, LUA_REGISTRYINDEX, client->rL_ref);
				return;
			}
			pos++;
		}
	}
}

static int pulsar_tcp_client_read(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (!client->active || client->closed) {
		lua_pushnil(L);
		lua_pushliteral(L, "client read not active");
		return 2;
	}

	int len = luaL_checknumber(L, 2);
	if (len == 0) {
		lua_pushliteral(L, "");
		return 1;
	}

	// No need to wait, we already have enough data
	if (len <= client->read_buf_pos) {
		lua_pushlstring(L, client->read_buf, len);
		char *newbuf = malloc(client->read_buf_len);
		if (client->read_buf_pos > len) memcpy(newbuf, client->read_buf + len, client->read_buf_pos - len);
		free(client->read_buf);
		client->read_buf = newbuf;
		client->read_buf_pos -= len;
		return 1;
	}

	lua_pushthread(L); client->rL = lua_tothread(L, -1); client->rL_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	client->read_wait_len = len;
	return lua_yield(L, 0);
}

static int pulsar_tcp_client_read_until(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (!client->active || client->closed) {
		lua_pushnil(L);
		lua_pushliteral(L, "client read not active");
		return 2;
	}

	size_t len;
	const char *until = luaL_checklstring(L, 2, &len);
	if (len < 1) {
		lua_pushnil(L);
		lua_pushliteral(L, "empty until");
		return 2;
	}
	size_t ignorelen = 0;
	const char *ignore = NULL;
	if (lua_isstring(L, 3)) ignore = lua_tolstring(L, 3, &ignorelen);

	// No need to wait, we may have enough data
	if (len <= client->read_buf_pos) {
		size_t pos = 0;
		while (pos <= client->read_buf_pos - len) {
			if (!memcmp(client->read_buf + pos, until, len)) {
				if (ignorelen && (ignorelen <= pos) && !memcmp(client->read_buf + pos - ignorelen, ignore, ignorelen))
					lua_pushlstring(L, client->read_buf, pos - ignorelen);
				else
					lua_pushlstring(L, client->read_buf, pos);
				char *newbuf = malloc(client->read_buf_len);
				if (client->read_buf_pos > pos + len) memcpy(newbuf, client->read_buf + pos + len, client->read_buf_pos - pos - len);
				free(client->read_buf);
				client->read_buf = newbuf;
				client->read_buf_pos -= pos + len;
				return 1;
			}
			pos++;
		}
	}

	lua_pushthread(L); client->rL = lua_tothread(L, -1); client->rL_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	client->read_wait_len = WAIT_LEN_UNTIL;
	client->read_wait_until = malloc((1+strlen(until)) * sizeof(char));
	strcpy(client->read_wait_until, until);
	client->read_wait_ignorelen = ignorelen;
	if (ignorelen) {
		client->read_wait_ignore = malloc((1+strlen(ignore)) * sizeof(char));
		strcpy(client->read_wait_ignore, ignore);
	}
	return lua_yield(L, 0);
}

static int pulsar_tcp_client_start(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (client->closed) return 0;
	uv_read_start((uv_stream_t*)client->sock, buf_alloc, tcp_client_read_cb);
	client->active = true;
	return 0;
}
static int pulsar_tcp_client_stop(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (client->closed) return 0;
	uv_read_stop((uv_stream_t*)client->sock);
	client->active = false;
	return 0;
}

static int pulsar_tcp_client_getpeername(lua_State *L)
{
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (client->closed) return 0;
	struct sockaddr_in peer;
	int peerlen = sizeof(peer);
	if (uv_tcp_getpeername(client->sock, (struct sockaddr*)&peer, &peerlen)) {
		lua_pushnil(L);
		lua_pushstring(L, "getpeername failed");
	} else {
		lua_pushstring(L, inet_ntoa(peer.sin_addr));
		lua_pushnumber(L, ntohs(peer.sin_port));
	}
	return 2;
}

static void tcp_client_connect_cb(uv_connect_t *_con, int status) {
	pulsar_tcp_client_connect *con = (pulsar_tcp_client_connect*)_con;
	lua_State *L = con->L;
	if (status) {
		uv_close((uv_handle_t*)con->sock, close_cb);
		free(con);
		lua_pushnil(L);
		lua_pushstring(L, "could not connect");
		pulsar_client_resume(NULL, L, 2);
		return;
	}


	// Initialize and start watcher to read client requests
	pulsar_tcp_client *client = (pulsar_tcp_client*)lua_newuserdata(L, sizeof(pulsar_tcp_client));
	pulsar_setmeta(L, MT_PULSAR_TCP_CLIENT);
	client->sock = con->sock;
	client->sock->data = client;
	client->loop = con->loop;
	client->closed = false;
	client->active = false;
	client->disconnected = false;

	client->read_wait_ignorelen = 0;
	client->read_wait_ignore = NULL;
	client->read_wait_len = 0;
	client->read_wait_until = NULL;
	client->read_buf_pos = 0;
	client->read_buf_len = DEFAULT_BUFFER_SIZE;
	client->read_buf = malloc(client->read_buf_len * sizeof(char));

	client->standalone = true;

	free(con);
	pulsar_client_resume(client, L, 1);
}

static int pulsar_tcp_client_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	const char *address = luaL_checkstring(L, 2);
	int port = luaL_checknumber(L, 3);

	pulsar_tcp_client_connect *req = (pulsar_tcp_client_connect*)malloc(sizeof(pulsar_tcp_client_connect));
	req->sock = malloc(uv_handle_size(UV_TCP));
	uv_tcp_init(loop->loop, req->sock);
	struct sockaddr_in addr;
	uv_ip4_addr(address, port, &addr);
	uv_tcp_connect((uv_connect_t*)req, req->sock, (const struct sockaddr*)&addr, tcp_client_connect_cb);

	req->loop = loop;

	lua_pushthread(L); req->L = lua_tothread(L, -1); req->L_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return lua_yield(L, 0);
}

/**************************************************************************************
 ** TCP Server calls
 **************************************************************************************/
static void tcp_server_accept_cb(uv_stream_t *_watcher, int status) {
	if (status == -1) {
		return;
	}

	pulsar_tcp_server *serv = (pulsar_tcp_server *)_watcher->data;

	// Initialize and start watcher to read client requests
	lua_rawgeti(serv->L, LUA_REGISTRYINDEX, serv->client_fct_ref);
	pulsar_tcp_client *client = (pulsar_tcp_client*)lua_newuserdata(serv->L, sizeof(pulsar_tcp_client));
	pulsar_setmeta(serv->L, MT_PULSAR_TCP_CLIENT);
	client->loop = serv->loop;
	client->sock = malloc(uv_handle_size(UV_TCP));
	uv_tcp_init(serv->loop->loop, client->sock);
	if (uv_accept(_watcher, (uv_stream_t*)client->sock)) {
		client->closed = true;
		uv_close((uv_handle_t*)client->sock, close_cb);
		lua_pop(serv->L, 2);
		return;
	}
	client->sock->data = client;
	client->active = false;
	client->disconnected = false;
	client->closed = false;

	client->read_wait_ignorelen = 0;
	client->read_wait_ignore = NULL;
	client->read_wait_len = 0;
	client->read_wait_until = NULL;
	client->read_buf_pos = 0;
	client->read_buf_len = DEFAULT_BUFFER_SIZE;
	client->read_buf = malloc(client->read_buf_len * sizeof(char));

	client->standalone = false;

	lua_State *L = lua_newthread(serv->L);
	lua_atpanic(L, pulsar_panic);
	client->co_ref = luaL_ref(serv->L, LUA_REGISTRYINDEX);
	lua_xmove(serv->L, L, 2);
	pulsar_client_resume(client, L, 1);
}

static int pulsar_tcp_server_close(lua_State *L) {
	pulsar_tcp_server *serv = (pulsar_tcp_server *)luaL_checkudata (L, 1, MT_PULSAR_TCP_SERVER);
	uv_close((uv_handle_t*)serv->sock, close_cb);
	serv->active = false;
	luaL_unref(L, LUA_REGISTRYINDEX, serv->client_fct_ref);
	return 0;
}
static int pulsar_tcp_server_start(lua_State *L) {
	pulsar_tcp_server *serv = (pulsar_tcp_server *)luaL_checkudata (L, 1, MT_PULSAR_TCP_SERVER);
	uv_listen((uv_stream_t*)serv->sock, 128, tcp_server_accept_cb);
	serv->active = true;
	return 0;
}

static int pulsar_tcp_server_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	const char *address = luaL_checkstring(L, 2);
	int port = luaL_checknumber(L, 3);
	if (!lua_isfunction(L, 4)) { lua_pushstring(L, "argument 3 is not a function"); lua_error(L); return 0; }

	struct sockaddr_in bind_addr;
	uv_ip4_addr(address, port, &bind_addr);

	// Initialize and start a watcher to accepts client requests
	pulsar_tcp_server *serv = (pulsar_tcp_server*)lua_newuserdata(L, sizeof(pulsar_tcp_server));
	pulsar_setmeta(L, MT_PULSAR_TCP_SERVER);
	serv->sock = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
	serv->sock->data = serv;
	uv_tcp_init(loop->loop, serv->sock);
	uv_tcp_bind(serv->sock, (const struct sockaddr*)&bind_addr);
	serv->L = L;
	serv->loop = loop;
	serv->active = false;

	lua_pushvalue(L, 4);
	serv->client_fct_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return 1;
}

/**************************************************************************************
 ** Timers
 **************************************************************************************/
static void pulsar_timer_resume(pulsar_timer *timer, lua_State *L, int nargs) {
	int ret = lua_resume(L, nargs);
	if (ret == LUA_ERRRUN) {
		printf("Error while running timer's coroutine: %s\n", lua_tostring(L, -1));
		traceback(L);

		if (timer->active) uv_timer_stop(timer->w_timeout);
		timer->active = false;
		return;
	}
}

static void timer_cb(uv_timer_t *_watcher, int status) {
	pulsar_timer *timer = (pulsar_timer *)_watcher->data;

	uv_timer_stop(timer->w_timeout);
	timer->active = false;
	if (timer->first_run) {
		timer->first_run = false;
		pulsar_timer_resume(timer, timer->L, 1);
	} else {
		pulsar_timer_resume(timer, timer->L, 0);
	}
}

static int pulsar_timer_close(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	if (timer->active) uv_timer_stop(timer->w_timeout);
	uv_close((uv_handle_t*)timer->w_timeout, close_cb);
	timer->active = false;
	luaL_unref(L, LUA_REGISTRYINDEX, timer->co_ref);
	return 0;
}
static int pulsar_timer_start(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	uv_timer_start(timer->w_timeout, timer_cb, timer->timeout, timer->repeat);
	timer->active = true;
	return 0;
}
static int pulsar_timer_stop(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	if (timer->active) uv_timer_stop(timer->w_timeout);
	timer->active = false;
	return 0;
}
static int pulsar_timer_next(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	uv_timer_again(timer->w_timeout);
	timer->active = true;
	return lua_yield(L, 0);
}

static int pulsar_timer_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	float first = luaL_checknumber(L, 2);
	float repeat = luaL_checknumber(L, 3);
	if (!lua_isfunction(L, 4)) { lua_pushstring(L, "argument 3 is not a function"); lua_error(L); return 0; }

	// Initialize and start a watcher to accepts client requests
	pulsar_timer *timer = (pulsar_timer*)lua_newuserdata(L, sizeof(pulsar_timer));
	pulsar_setmeta(L, MT_PULSAR_TIMER);
	timer->loop = loop;
	timer->active = false;
	timer->first_run = true;

	timer->L = lua_newthread(L);
	lua_atpanic(timer->L, pulsar_panic);
	lua_pushvalue(L, 4);
	lua_pushvalue(L, 5);
	lua_xmove(L, timer->L, 2);

	timer->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	timer->timeout = (int)(first * 1000);
	timer->repeat = (int)(repeat * 1000);

	timer->w_timeout = (uv_timer_t*)malloc(sizeof(uv_timer_t));
	timer->w_timeout->data = timer;
	uv_timer_init(loop->loop, timer->w_timeout);
	return 1;
}

/**************************************************************************************
 ** Idles
 **************************************************************************************/
static void pulsar_idle_resume(pulsar_idle *idle, lua_State *L, int nargs) {
	int ret = lua_resume(L, nargs);
	// More to do
	if (ret == LUA_YIELD) return;

	// Finished, end the idle
	if (!ret) {
		uv_idle_stop(idle->w_timeout);
		idle->active = false;
		return;
	}

	if (ret == LUA_ERRRUN) {
		printf("Error while running idle's coroutine: %s\n", lua_tostring(L, -1));
		traceback(L);

		uv_idle_stop(idle->w_timeout);
		idle->active = false;
		return;
	}
}

static void idle_cb(uv_idle_t *_watcher, int status) {
	pulsar_idle *idle = (pulsar_idle *)_watcher->data;

	if (idle->first_run) {
		idle->first_run = false;
		pulsar_idle_resume(idle, idle->L, 1);
	} else {
		pulsar_idle_resume(idle, idle->L, 0);
	}
}

static int pulsar_idle_close(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	uv_idle_stop(idle->w_timeout);
	uv_close((uv_handle_t*)idle->w_timeout, close_cb);
	idle->active = false;
	luaL_unref(L, LUA_REGISTRYINDEX, idle->co_ref);
	return 0;
}
static int pulsar_idle_start(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	uv_idle_start(idle->w_timeout, idle_cb);
	idle->active = true;
	return 0;
}
static int pulsar_idle_stop(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	uv_idle_stop(idle->w_timeout);
	idle->active = false;
	return 0;
}
static int pulsar_idle_next(lua_State *L) {
	return lua_yield(L, 0);
}

static int pulsar_idle_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	if (!lua_isfunction(L, 2)) { lua_pushstring(L, "argument 1 is not a function"); lua_error(L); return 0; }

	// Initialize and start a watcher to accepts client requests
	pulsar_idle *idle = (pulsar_idle*)lua_newuserdata(L, sizeof(pulsar_idle));
	pulsar_setmeta(L, MT_PULSAR_IDLE);
	idle->loop = loop;
	idle->active = false;
	idle->first_run = true;

	idle->L = lua_newthread(L);
	lua_atpanic(idle->L, pulsar_panic);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_xmove(L, idle->L, 2);

	idle->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	idle->w_timeout = (uv_idle_t*)malloc(sizeof(uv_idle_t));
	idle->w_timeout->data = idle;
	uv_idle_init(loop->loop, idle->w_timeout);
	return 1;
}

/**************************************************************************************
 ** Idle Workers
 **************************************************************************************/
static void pulsar_idle_worker_resume(pulsar_idle_worker *idle_worker, lua_State *L, int nargs) {
	int ret = lua_resume(L, nargs);
	// More to do
	if (ret == LUA_YIELD) return;

	// Finished, end the idle_worker
	if (!ret) {
		uv_idle_stop(idle_worker->w_timeout);
		idle_worker->active = false;
		return;
	}

	if (ret == LUA_ERRRUN) {
		printf("Error while running idle_worker's coroutine: %s\n", lua_tostring(L, -1));
		traceback(L);

		uv_idle_stop(idle_worker->w_timeout);
		idle_worker->active = false;
		return;
	}
}

static void idle_worker_cb(uv_idle_t *_watcher, int status) {
	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)_watcher->data;
	if (!idle_worker->chain) {
		uv_idle_stop(idle_worker->w_timeout);
		idle_worker->active = false;
		return;
	}

	pulsar_idle_worker_chain *chain = idle_worker->chain;
	idle_worker->chain = idle_worker->chain->next;
	lua_State *rL = chain->L;
	luaL_unref(rL, LUA_REGISTRYINDEX, chain->L_ref);
	int nargs = chain->nargs;
	free(chain);

	pulsar_idle_worker_resume(idle_worker, rL, nargs);
}

static int pulsar_idle_worker_close(lua_State *L) {
	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)luaL_checkudata (L, 1, MT_PULSAR_IDLE_WORKER);
	uv_idle_stop(idle_worker->w_timeout);
	idle_worker->active = false;
	while (idle_worker->chain) {
		pulsar_idle_worker_chain *chain = idle_worker->chain;
		idle_worker->chain = idle_worker->chain->next;

		luaL_unref(L, LUA_REGISTRYINDEX, chain->L_ref);
		free(chain);
	}
	uv_close((uv_handle_t*)idle_worker->w_timeout, close_cb);
	return 0;
}

static int pulsar_idle_worker_split(lua_State *L) {
	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)luaL_checkudata (L, 1, MT_PULSAR_IDLE_WORKER);
	uv_idle_start(idle_worker->w_timeout, idle_worker_cb);
	idle_worker->active = true;

	pulsar_idle_worker_chain *chain = malloc(sizeof(pulsar_idle_worker_chain));
	lua_pushthread(L); chain->L = lua_tothread(L, -1); chain->L_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	chain->nargs = 0;
	chain->next = NULL;

	if (!idle_worker->chain) idle_worker->chain = chain;
	else {
		pulsar_idle_worker_chain *tail = idle_worker->chain;
		while (tail->next) tail = tail->next;
		tail->next = chain;
	}

	return lua_yield(L, 0);
}

static int pulsar_idle_worker_register(lua_State *L) {
	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)luaL_checkudata (L, 1, MT_PULSAR_IDLE_WORKER);
	if (!lua_isfunction(L, 2)) { lua_pushstring(L, "argument 1 is not a function"); lua_error(L); return 0; }

	uv_idle_start(idle_worker->w_timeout, idle_worker_cb);
	idle_worker->active = true;

	pulsar_idle_worker_chain *chain = malloc(sizeof(pulsar_idle_worker_chain));
	chain->L = lua_newthread(L);
	chain->L_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	chain->nargs = 0;
	lua_atpanic(chain->L, pulsar_panic);
	lua_pushvalue(L, 2);
	lua_xmove(L, chain->L, 1);

	chain->next = NULL;

	if (!idle_worker->chain) idle_worker->chain = chain;
	else {
		pulsar_idle_worker_chain *tail = idle_worker->chain;
		while (tail->next) tail = tail->next;
		tail->next = chain;
	}
	return 0;
}

static int pulsar_idle_worker_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);

	// Initialize and start a watcher to accepts client requests
	pulsar_idle_worker *idle = (pulsar_idle_worker*)lua_newuserdata(L, sizeof(pulsar_idle_worker));
	pulsar_setmeta(L, MT_PULSAR_IDLE_WORKER);
	idle->loop = loop;
	idle->active = false;
	idle->chain = NULL;

	idle->w_timeout = (uv_idle_t*)malloc(sizeof(uv_idle_t));
	idle->w_timeout->data = idle;
	uv_idle_init(loop->loop, idle->w_timeout);
	return 1;
}


/**************************************************************************************
 ** Spawns
 **************************************************************************************/

static void writeTblFixed(pulsar_spawn_ret *s, const char *data, long len) {
	if (len + s->bufpos + 1 >= s->buflen) {
		s->buf = realloc(s->buf, (len + s->bufpos + 1) * sizeof(char));
		s->buflen = len + s->bufpos + 1;
	}
	memcpy(s->buf + s->bufpos, data, len);
	s->bufpos += len;
}
#define writeTbl(s, data) { writeTblFixed(s, data, strlen(data)); }

static void tbl_dump_string(pulsar_spawn_ret *s, const char *str, size_t l)
{
	while (l--) {
		switch (*str) {
		case '"': case '\\': case '\n': {
			writeTblFixed(s, "\\", 1);
			writeTblFixed(s, str, 1);
			break;
		}
		case '\r': {
			writeTblFixed(s, "\\r", 2);
			break;
		}
		case '\0': {
			writeTblFixed(s, "\\000", 4);
			break;
		}
		default: {
			writeTblFixed(s, str, 1);
			break;
		}
		}
		str++;
	}
}

static int tbl_dump_function(lua_State *L, const void* p, size_t sz, void* ud)
{
	pulsar_spawn_ret *s = (pulsar_spawn_ret*)ud;
	tbl_dump_string(s, p, sz);
	return 0;
}

static void tbl_basic_serialize(lua_State *L, pulsar_spawn_ret *s, int type, int idx)
{
	if (type == LUA_TNIL) {
		writeTblFixed(s, "nil", 3);
	} else if (type == LUA_TBOOLEAN) {
		if (lua_toboolean(L, idx)) { writeTblFixed(s, "true", 4); }
		else { writeTblFixed(s, "false", 5); }
	} else if (type == LUA_TNUMBER) {
		lua_pushvalue(L, idx);
		size_t len;
		const char *n = lua_tolstring(L, -1, &len);
		writeTblFixed(s, n, len);
		lua_pop(L, 1);
	} else if (type == LUA_TSTRING) {
		size_t len;
		const char *str = lua_tolstring(L, idx, &len);
		writeTblFixed(s, "\"", 1);
		tbl_dump_string(s, str, len);
		writeTblFixed(s, "\"", 1);
	} else if (type == LUA_TFUNCTION) {
		writeTblFixed(s, "loadstring(\"", 12);
		lua_dump(L, tbl_dump_function, s);
		writeTblFixed(s, "\")", 2);
	} else if (type == LUA_TTABLE) {
		int ktype, etype;

		writeTblFixed(s, "{", 1);
		/* table is in the stack at index 't' */
		lua_pushnil(L);  /* first key */

		while (lua_next(L, idx - 1) != 0)
		{
			ktype = lua_type(L, -2);
			etype = lua_type(L, -1);

			// Only save allowed types
			if (
				((ktype == LUA_TBOOLEAN) || (ktype == LUA_TNUMBER) || (ktype == LUA_TSTRING) || (ktype == LUA_TFUNCTION) || (ktype == LUA_TTABLE)) &&
				((etype == LUA_TBOOLEAN) || (etype == LUA_TNUMBER) || (etype == LUA_TSTRING) || (etype == LUA_TFUNCTION) || (etype == LUA_TTABLE))
				)
			{
				writeTblFixed(s, "[", 1);
				tbl_basic_serialize(L, s, ktype, -2);
				writeTblFixed(s, "]=", 2);
				tbl_basic_serialize(L, s, etype, -1);
				writeTblFixed(s, ",\n", 2);
			}

			/* removes 'value'; keeps 'key' for next iteration */
			lua_pop(L, 1);
		}
		writeTblFixed(s, "}\n", 2);
	} else {
		printf("*WARNING* can not pass to/from spawn a value of type %s\n", lua_typename(L, type));
	}
}

const char *spawn_ret_read(lua_State *L, void *data, size_t *size) {
	pulsar_spawn_ret *ret = (pulsar_spawn_ret *)data;
	if (!ret->bufpos) return NULL;
	*size = ret->bufpos;
	ret->bufpos = 0;
	return ret->buf;
}

static void spawn_cb(uv_work_t *_watcher, int status) {
	pulsar_spawn *spawn = (pulsar_spawn *)_watcher;

	// We make a new coroutine to run the return function inside because the calling coroutine is currently paused and cant be used to call functions
	pulsar_spawn_ret *ret = &spawn->ret;
	lua_State *L = lua_newthread(spawn->L);
	lua_load(L, spawn_ret_read, ret, "spawned code return");
	lua_pcall(L, 0, ret->nbrets, 0);
	free(ret->buf);
	lua_xmove(L, spawn->L, ret->nbrets);
	lua_remove(spawn->L, -ret->nbrets - 1);

	int res = lua_resume(spawn->L, ret->nbrets);
	luaL_unref(spawn->L, LUA_REGISTRYINDEX, spawn->L_ref);
	free(spawn);
	if (res == LUA_ERRRUN) {
		printf("Error while running spawn's callback: %s\n", lua_tostring(spawn->L, -1));
		traceback(spawn->L);
	}
}

const char *spawn_read(lua_State *L, void *data, size_t *size) {
	pulsar_spawn *spawn = (pulsar_spawn *)data;
	if (!spawn->fctcode_len) return NULL;
	*size = spawn->fctcode_len;
	spawn->fctcode_len = 0;
	return spawn->fctcode;
}

static void spawn_exec(uv_work_t *req) {
	pulsar_spawn *spawn = (pulsar_spawn *)req;
	lua_State *L = luaL_newstate();
	luaL_openlibs(L);
	lua_pushcfunction(L, traceback);  /* push traceback function */
	int base = lua_gettop(L);

	// Push main function
	lua_load(L, spawn_read, spawn, "spawned code");
	free(spawn->fctcode);

	// Push args function and call it to get the args
	lua_load(L, spawn_ret_read, &spawn->arg, "spawned code args");
	free(spawn->arg.buf);
	lua_pcall(L, 0, spawn->arg.nbrets, 0);

	// Call the main functions with the args
	lua_pcall(L, spawn->arg.nbrets, LUA_MULTRET, base);
	
	// Count & serialize returns
	int nbrets = lua_gettop(L) - base;
	pulsar_spawn_ret *ret = &spawn->ret;
	ret->buf = malloc(sizeof(char) * 32);
	ret->buflen = 32;
	ret->buf[0] = 'r'; ret->buf[1] = 'e'; ret->buf[2] = 't'; ret->buf[3] = 'u'; ret->buf[4] = 'r'; ret->buf[5] = 'n'; ret->buf[6] = ' ';
	ret->bufpos = 7;
	
	if (nbrets > 0) {
		ret->nbrets = nbrets;
		while (nbrets > 0) {
			tbl_basic_serialize(L, ret, lua_type(L, -nbrets), -nbrets);
			nbrets--;
			if (nbrets) writeTblFixed(ret, ",", 1);
		}
		lua_pop(L, nbrets);
	} else {
		ret->nbrets = 1;
		ret->buf[7] = 'n'; ret->buf[8] = 'i'; ret->buf[9] = 'l';
		ret->bufpos = 10;
	}
	ret->buf[ret->bufpos] = '\0';

	lua_remove(L, base);  /* remove traceback function */
	lua_close(L);
}

static int spawn_dump(lua_State *L, const void* p, size_t sz, void* ud)
{
	pulsar_spawn_base *sbase = (pulsar_spawn_base*)ud;
	sbase->fctcode = realloc(sbase->fctcode, sbase->fctcode_len + sz);
	memcpy(sbase->fctcode + sbase->fctcode_len, p, sz);
	sbase->fctcode_len += sz;
	return 0;
}

static int pulsar_spawn_call(lua_State *L)
{
	pulsar_spawn_base *sbase = (pulsar_spawn_base *)luaL_checkudata (L, 1, MT_PULSAR_SPAWN);
	pulsar_spawn *spawn = (pulsar_spawn*)malloc(sizeof(pulsar_spawn));

	pulsar_spawn_ret *ret = &spawn->arg;
	ret->buf = malloc(sizeof(char) * 32);
	ret->buflen = 32;
	ret->buf[0] = 'r'; ret->buf[1] = 'e'; ret->buf[2] = 't'; ret->buf[3] = 'u'; ret->buf[4] = 'r'; ret->buf[5] = 'n'; ret->buf[6] = ' ';
	ret->bufpos = 7;
	
	int nbrets = lua_gettop(L) - 1;
	if (nbrets > 0) {
		ret->nbrets = nbrets;
		while (nbrets > 0) {
			tbl_basic_serialize(L, ret, lua_type(L, -nbrets), -nbrets);
			nbrets--;
			if (nbrets) writeTblFixed(ret, ",", 1);
		}
		lua_pop(L, nbrets);
	} else {
		ret->nbrets = 1;
		ret->buf[7] = 'n'; ret->buf[8] = 'i'; ret->buf[9] = 'l';
		ret->bufpos = 10;
	}
	ret->buf[ret->bufpos] = '\0';


	spawn->fctcode = (char*)malloc(sbase->fctcode_len);
	memcpy(spawn->fctcode, sbase->fctcode, sbase->fctcode_len);
	spawn->fctcode_len = sbase->fctcode_len;
	lua_pushthread(L); spawn->L = lua_tothread(L, -1); spawn->L_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	uv_queue_work(sbase->loop->loop, (uv_work_t*)&spawn->work, spawn_exec, spawn_cb);
	return lua_yield(L, 0);
}

static int pulsar_spawn_free(lua_State *L)
{
	pulsar_spawn_base *sbase = (pulsar_spawn_base *)luaL_checkudata (L, 1, MT_PULSAR_SPAWN);
	if (sbase->fctcode) free(sbase->fctcode);
	return 0;
}

static int pulsar_spawn_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	if (!lua_isfunction(L, 2)) { lua_pushstring(L, "argument 1 is not a function"); lua_error(L); return 0; }

	// Initialize and start a watcher to accepts client requests
	pulsar_spawn_base *sbase = (pulsar_spawn_base*)lua_newuserdata(L, sizeof(pulsar_spawn_base));
	pulsar_setmeta(L, MT_PULSAR_SPAWN);
	sbase->loop = loop;
	sbase->fctcode = NULL;
	sbase->fctcode_len = 0;
	lua_pushvalue(L, 2);
	lua_dump(L, spawn_dump, sbase);
	lua_pop(L, 1);

	return 1;
}

/**************************************************************************************
 ** Loop calls
 **************************************************************************************/
static int main_loop_ref = LUA_NOREF;
static int pulsar_loop_default(lua_State *L)
{
	if (main_loop_ref == LUA_NOREF) {
		pulsar_loop *loop = (pulsar_loop*)lua_newuserdata(L, sizeof(pulsar_loop));
		pulsar_setmeta(L, MT_PULSAR_LOOP);
		loop->loop = uv_default_loop();
		lua_pushvalue(L, -1);
		main_loop_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		lua_rawgeti(L, LUA_REGISTRYINDEX, main_loop_ref);
	}
	return 1;
}

static int pulsar_loop_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop*)lua_newuserdata(L, sizeof(pulsar_loop));
	pulsar_setmeta(L, MT_PULSAR_LOOP);
	loop->loop = uv_loop_new();
	return 1;
}

static int pulsar_loop_close(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	uv_loop_delete(loop->loop);
	return 0;
}

static int pulsar_loop_run(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	uv_run(loop->loop, 0);
	return 0;
}

/**************************************************************************************
 ** Global things
 **************************************************************************************/
static const struct luaL_reg meth_pulsar_loop[] =
{
	{"run", pulsar_loop_run},
	{"tcpServer", pulsar_tcp_server_new},
	{"tcpClient", pulsar_tcp_client_new},
	{"timer", pulsar_timer_new},
	{"idle", pulsar_idle_new},
	{"worker", pulsar_idle_worker_new},
	{"longTask", pulsar_idle_worker_new},
	{"spawn", pulsar_spawn_new},
	{"close", pulsar_loop_close},
	{"__gc", pulsar_loop_close},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_timer[] =
{
	{"start", pulsar_timer_start},
	{"stop", pulsar_timer_stop},
	{"next", pulsar_timer_next},
	{"close", pulsar_timer_close},
	{"__gc", pulsar_timer_close},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_idle[] =
{
	{"start", pulsar_idle_start},
	{"stop", pulsar_idle_stop},
	{"next", pulsar_idle_next},
	{"close", pulsar_idle_close},
	{"__gc", pulsar_idle_close},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_idle_worker[] =
{
	{"register", pulsar_idle_worker_register},
	{"split", pulsar_idle_worker_split},
	{"close", pulsar_idle_worker_close},
	{"__gc", pulsar_idle_worker_close},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_spawn[] =
{
	{"__call", pulsar_spawn_call},
	{"__gc", pulsar_spawn_free},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_tcp_server[] =
{
	{"start", pulsar_tcp_server_start},
	{"close", pulsar_tcp_server_close},
	{"__gc", pulsar_tcp_server_close},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_tcp_client[] =
{
	{"startRead", pulsar_tcp_client_start},
	{"stopRead", pulsar_tcp_client_stop},
	{"recv", pulsar_tcp_client_read},
	{"read", pulsar_tcp_client_read},
	{"recvUntil", pulsar_tcp_client_read_until},
	{"readUntil", pulsar_tcp_client_read_until},
	{"send", pulsar_tcp_client_send},
	{"connected", pulsar_tcp_client_is_connected},
	{"hasData", pulsar_tcp_client_has_data},
	{"getpeername", pulsar_tcp_client_getpeername},
	{"close", pulsar_tcp_client_close},
	{"__gc", pulsar_tcp_client_close},
	{NULL, NULL},
};

static const struct luaL_reg pulsarlib[] =
{
	{"defaultLoop", pulsar_loop_default},
	{"newLoop", pulsar_loop_new},
	{NULL, NULL},
};

/*
** Create a metatable and leave it on top of the stack.
*/
static void pulsar_createmeta(lua_State *L, const char *name, const luaL_Reg *methods) {
	if (!luaL_newmetatable (L, name)) return;

	/* define methods */
	luaL_setfuncs (L, methods, 0);

	/* define metamethods */
	lua_pushliteral (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushliteral (L, "__metatable");
	lua_pushliteral (L, "you're not allowed to get this metatable");
	lua_settable (L, -3);

	lua_pop(L, 1);
}

/*
** Assumes the table is on top of the stack.
*/
static void set_info (lua_State *L)
{
	lua_pushliteral (L, "_COPYRIGHT");
	lua_pushliteral (L, "Copyright (C) 2013 Nicolas Casalini");
	lua_settable (L, -3);
	lua_pushliteral (L, "_VERSION");
	lua_pushliteral (L, "Pulsar 1.0.0");
	lua_settable (L, -3);
}

int luaopen_pulsar(lua_State *L)
{
	signal(SIGPIPE, SIG_IGN);
	lua_atpanic(L, pulsar_panic_main);

	pulsar_createmeta(L, MT_PULSAR_LOOP, meth_pulsar_loop);
	pulsar_createmeta(L, MT_PULSAR_TIMER, meth_pulsar_timer);
	pulsar_createmeta(L, MT_PULSAR_IDLE, meth_pulsar_idle);
	pulsar_createmeta(L, MT_PULSAR_IDLE_WORKER, meth_pulsar_idle_worker);
	pulsar_createmeta(L, MT_PULSAR_TCP_SERVER, meth_pulsar_tcp_server);
	pulsar_createmeta(L, MT_PULSAR_TCP_CLIENT, meth_pulsar_tcp_client);
	pulsar_createmeta(L, MT_PULSAR_SPAWN, meth_pulsar_spawn);

	luaL_openlib(L, "pulsar", pulsarlib, 0);
	set_info(L);
	return 1;
}
