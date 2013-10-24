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

static bool set_non_blocking(int fd) {
	long on = 1L;
	if (ioctl(fd, (int)FIONBIO, (char *)&on))
	{
		printf("ioctl FIONBIO call failed on %d\n", fd);
		return false;
	}
	return true;
}

#define DEFAULT_BUFFER_SIZE	1024

#define MT_PULSAR_LOOP		"Pulsar Loop"
#define MT_PULSAR_TIMER		"Pulsar Timer"
#define MT_PULSAR_IDLE		"Pulsar Idle"
#define MT_PULSAR_IDLE_WORKER	"Pulsar Idle Worker"
#define MT_PULSAR_TCP_SERVER	"Pulsar TCP Server"
#define MT_PULSAR_TCP_CLIENT	"Pulsar TCP Client"

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

/**************************************************************************************
 ** TCP Client calls
 **************************************************************************************/
static void pulsar_client_resume(pulsar_tcp_client *client, lua_State *L, int nargs) {
	int ret = lua_resume(L, nargs);
	// More to do
	if (ret == LUA_YIELD) return;

	// Finished, end the client
	if (!ret) {
		ev_io_stop(client->loop->loop, &client->w_read);
		ev_io_stop(client->loop->loop, &client->w_send);
		client->active = false;
		client->disconnected = true;
		close(client->fd);
		client->fd = 0;
		// Let the rest free up by GC
		return;
	}

	if (ret == LUA_ERRRUN) {
		printf("Error while running client's coroutine (fd %d): %s\n", client->fd, lua_tostring(L, -1));
		traceback(L);

		ev_io_stop(client->loop->loop, &client->w_read);
		ev_io_stop(client->loop->loop, &client->w_send);
		client->active = false;
		client->disconnected = true;
		close(client->fd);
		client->fd = 0;
		// Let the rest free up by GC
		return;
	}
}

static int pulsar_tcp_client_close(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	ev_io_stop(client->loop->loop, &client->w_read);
	ev_io_stop(client->loop->loop, &client->w_send);
	client->active = false;
	client->disconnected = true;
	if (client->fd) close(client->fd);
	if (client->read_buf) free(client->read_buf);
	client->read_buf = NULL;
	client->send_buf_pos = 0;
	if (client->send_buf) free(client->send_buf);
	while (client->send_buf_chain) {
		pulsar_tcp_client_send_chain *chain = client->send_buf_chain;
		client->send_buf_chain = client->send_buf_chain->next;
		free(chain->send_buf);
		free(chain);
	}
	client->send_buf_chain = NULL;
	return 0;
}

static int pulsar_tcp_client_start(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	ev_io_start(client->loop->loop, &client->w_read);
	client->active = true;
	return 0;
}
static int pulsar_tcp_client_stop(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	ev_io_stop(client->loop->loop, &client->w_read);
	client->active = false;
	return 0;
}

static int pulsar_tcp_client_is_connected(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	lua_pushboolean(L, client->active && !client->disconnected);
	return 1;
}
static int pulsar_tcp_client_has_data(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	lua_pushboolean(L, client->read_buf_pos > 0);
	return 1;
}

static int pulsar_tcp_client_send(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	size_t datalen;
	const char *data = lua_tolstring(L, 2, &datalen);
	bool nowait = lua_toboolean(L, 3);

	if (client->send_buf_len) {
		pulsar_tcp_client_send_chain *chain = malloc(sizeof(pulsar_tcp_client_send_chain));
		chain->send_buf = malloc(datalen);
		memcpy(chain->send_buf, data, datalen);
		chain->send_buf_len = datalen;
		chain->send_buf_nowait = nowait;
		lua_pushthread(L); chain->sL = lua_tothread(L, -1); lua_pop(L, 1);
		chain->next = NULL;

		if (!client->send_buf_chain) client->send_buf_chain = chain;
		else {
			pulsar_tcp_client_send_chain *tail = client->send_buf_chain;
			while (tail->next) tail = tail->next;
			tail->next = chain;
		}

		if (nowait) return 0;
		else return lua_yield(L, 0);
	}

	ev_io_start(client->loop->loop, &client->w_send);

	lua_pushthread(L); client->sL = lua_tothread(L, -1); lua_pop(L, 1);
	client->send_buf_nowait = nowait;
	client->send_buf_len = datalen;
	client->send_buf_pos = 0;
	client->send_buf = malloc(datalen);
	memcpy(client->send_buf, data, datalen);

	if (nowait) return 0;
	else return lua_yield(L, 0);
}

static void tcp_client_send_cb(struct ev_loop *loop, struct ev_io *_watcher, int revents){
	if (EV_ERROR & revents) return;

	pulsar_tcp_client *client = (pulsar_tcp_client *) (((char *)_watcher) - offsetof(pulsar_tcp_client, w_send));

	if ((!client->send_buf_len) || (client->send_buf_pos >= client->send_buf_len)) {
		ev_io_stop(loop, &client->w_send);
		return;
	}

	ssize_t len = write(client->fd, client->send_buf + client->send_buf_pos, client->send_buf_len - client->send_buf_pos);

	if ((len < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK))
	{
		client->send_buf_len = 0;
		free(client->send_buf);
		ev_io_stop(loop, &client->w_send);
		client->disconnected = true;
		lua_pushboolean(client->sL, false);
		pulsar_client_resume(client, client->sL, 1);
		return;
	}

	if (len == 0)
	{
		client->send_buf_len = 0;
		free(client->send_buf);
		ev_io_stop(loop, &client->w_send);
		client->disconnected = true;
		lua_pushboolean(client->sL, false);
		pulsar_client_resume(client, client->sL, 1);
		return;
	}

	client->send_buf_pos += len;

	if (client->send_buf_pos >= client->send_buf_len) {
		bool nowait = client->send_buf_nowait;
		client->send_buf_len = 0;
		free(client->send_buf);
		client->send_buf = NULL;

		/* Grab the next if any */
		if (client->send_buf_chain) {
			pulsar_tcp_client_send_chain *chain = client->send_buf_chain;
			client->send_buf_chain = client->send_buf_chain->next;

			client->send_buf_pos = 0;
			client->send_buf_len = chain->send_buf_len;
			client->send_buf = chain->send_buf;
			client->send_buf_nowait = chain->send_buf_nowait;
			free(chain);
		}
		else ev_io_stop(loop, &client->w_send);

		if (!nowait) {
			lua_pushboolean(client->sL, true);
			pulsar_client_resume(client, client->sL, 1);
		}
		return;
	}
}

static int pulsar_tcp_client_read(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (!client->active) {
		lua_pushnil(L);
		lua_pushliteral(L, "client read not active");
		return 2;
	}

	int len = luaL_checknumber(L, 2);
	if (len == 0) {
		lua_pushliteral(L, "");
		return 1;
	}

	/* No need to wait, we already have enough data */
	if (len <= client->read_buf_pos) {
		lua_pushlstring(L, client->read_buf, len);
		char *newbuf = malloc(client->read_buf_len);
		if (client->read_buf_pos > len) memcpy(newbuf, client->read_buf + len, client->read_buf_pos);
		free(client->read_buf);
		client->read_buf = newbuf;
		client->read_buf_pos -= len;
		return 1;
	}

	lua_pushthread(L); client->rL = lua_tothread(L, -1); lua_pop(L, 1);
	client->read_wait_len = len;
	return lua_yield(L, 0);
}

static int pulsar_tcp_client_read_until(lua_State *L) {
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);
	if (!client->active) {
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

	/* No need to wait, we may have enough data */
	if (len <= client->read_buf_pos) {
		size_t pos = 0;
		while (pos <= client->read_buf_pos - len) {
			if (!memcmp(client->read_buf + pos, until, len)) {
				if (ignorelen && (ignorelen <= pos) && !memcmp(client->read_buf + pos - ignorelen, ignore, ignorelen))
					lua_pushlstring(L, client->read_buf, pos - ignorelen);
				else
					lua_pushlstring(L, client->read_buf, pos);
				char *newbuf = malloc(client->read_buf_len);
				if (client->read_buf_pos > pos + len) memcpy(newbuf, client->read_buf + pos + len, client->read_buf_pos);
				free(client->read_buf);
				client->read_buf = newbuf;
				client->read_buf_pos -= pos + len;
				return 1;
			}
			pos++;
		}
	}

	lua_pushthread(L); client->rL = lua_tothread(L, -1); lua_pop(L, 1);
	client->read_wait_len = WAIT_LEN_UNTIL;
	client->read_wait_until = strdup(until);
	client->read_wait_ignorelen = ignorelen;
	if (ignorelen) client->read_wait_ignore = strdup(ignore);
	return lua_yield(L, 0);
}

static void tcp_client_read_cb(struct ev_loop *loop, struct ev_io *_watcher, int revents){
	if (EV_ERROR & revents) return;

	pulsar_tcp_client *client = (pulsar_tcp_client *)_watcher;

	if (client->read_buf_len - client->read_buf_pos < DEFAULT_BUFFER_SIZE / 4) {
		char *newbuf = malloc(client->read_buf_len * 2);
		memcpy(newbuf, client->read_buf, client->read_buf_pos);
		free(client->read_buf);
		client->read_buf = newbuf;
		client->read_buf_len *= 2;
	}

	ssize_t read = recv(client->fd, client->read_buf + client->read_buf_pos, client->read_buf_len - client->read_buf_pos, 0);

	if ((read < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK))
	{
		ev_io_stop(loop, &client->w_read);
		client->active = false;
		client->disconnected = true;
		lua_pushnil(client->rL);
		lua_pushliteral(client->rL, "disconnected");
		pulsar_client_resume(client, client->rL, 2);
		return;
	}

	if (read == 0)
	{
		ev_io_stop(loop, &client->w_read);
		client->active = false;
		client->disconnected = true;
		lua_pushnil(client->rL);
		lua_pushliteral(client->rL, "disconnected");
		pulsar_client_resume(client, client->rL, 2);
		return;
	}
	client->read_buf_pos += read;

	if ((client->read_wait_len > 0) && (client->read_buf_pos >= client->read_wait_len)) {
		lua_pushlstring(client->rL, client->read_buf, client->read_wait_len);
		char *newbuf = malloc(client->read_buf_len);
		if (client->read_buf_pos > client->read_wait_len) memcpy(newbuf, client->read_buf + client->read_wait_len, client->read_buf_pos);
		free(client->read_buf);
		client->read_buf = newbuf;
		client->read_buf_pos -= client->read_wait_len;
		client->read_wait_len = 0;

		pulsar_client_resume(client, client->rL, 1);
	}

	if (client->read_wait_len == WAIT_LEN_UNTIL) {
		size_t len = strlen(client->read_wait_until);
		const char *until = client->read_wait_until;
		size_t ignorelen = client->read_wait_ignorelen;
		const char *ignore = client->read_wait_ignore;
		size_t pos = 0;
		while (pos <= client->read_buf_pos - len) {
			if (!memcmp(client->read_buf + pos, until, len)) {
				if (ignorelen && (ignorelen <= pos) && !memcmp(client->read_buf + pos - ignorelen, ignore, ignorelen))
					lua_pushlstring(client->rL, client->read_buf, pos - ignorelen);
				else
					lua_pushlstring(client->rL, client->read_buf, pos);

				char *newbuf = malloc(client->read_buf_len);
				if (client->read_buf_pos > pos + len) memcpy(newbuf, client->read_buf + pos + len, client->read_buf_pos);
				free(client->read_buf);
				client->read_buf = newbuf;
				client->read_buf_pos -= pos + len;
				client->read_wait_len = 0;
				free(client->read_wait_until);
				if (client->read_wait_ignorelen) free(client->read_wait_ignore);
				client->read_wait_ignorelen = 0;
				client->read_wait_until = NULL;

				pulsar_client_resume(client, client->rL, 1);
				return;
			}
			pos++;
		}
	}
}

static int pulsar_tcp_client_getpeername(lua_State *L)
{
	pulsar_tcp_client *client = (pulsar_tcp_client *)luaL_checkudata (L, 1, MT_PULSAR_TCP_CLIENT);

	struct sockaddr_in peer;
	socklen_t peer_len = sizeof(peer);
	if (getpeername(client->fd, (struct sockaddr *) &peer, &peer_len) < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "getpeername failed");
	} else {
		lua_pushstring(L, inet_ntoa(peer.sin_addr));
		lua_pushnumber(L, ntohs(peer.sin_port));
	}
	return 2;
}

/**************************************************************************************
 ** TCP Server calls
 **************************************************************************************/
static int pulsar_tcp_server_close(lua_State *L) {
	pulsar_tcp_server *serv = (pulsar_tcp_server *)luaL_checkudata (L, 1, MT_PULSAR_TCP_SERVER);
	ev_io_stop(serv->loop->loop, &serv->w_accept);
	serv->active = false;
	if (serv->fd) close(serv->fd);
	serv->fd = 0;
	luaL_unref(L, LUA_REGISTRYINDEX, serv->client_fct_ref);
	return 0;
}
static int pulsar_tcp_server_start(lua_State *L) {
	pulsar_tcp_server *serv = (pulsar_tcp_server *)luaL_checkudata (L, 1, MT_PULSAR_TCP_SERVER);
	ev_io_start(serv->loop->loop, &serv->w_accept);
	serv->active = true;
	return 0;
}
static int pulsar_tcp_server_stop(lua_State *L) {
	pulsar_tcp_server *serv = (pulsar_tcp_server *)luaL_checkudata (L, 1, MT_PULSAR_TCP_SERVER);
	ev_io_stop(serv->loop->loop, &serv->w_accept);
	serv->active = false;
	return 0;
}

static void tcp_server_accept_cb(struct ev_loop *loop, struct ev_io *_watcher, int revents) {
	if (EV_ERROR & revents) return;

	pulsar_tcp_server *serv = (pulsar_tcp_server *)_watcher;

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	int fd = accept(serv->fd, (struct sockaddr *)&client_addr, &client_len);
	if (fd < 0) return;
	if (!set_non_blocking(fd)) return;

	// Initialize and start watcher to read client requests
	lua_rawgeti(serv->L, LUA_REGISTRYINDEX, serv->client_fct_ref);
	pulsar_tcp_client *client = (pulsar_tcp_client*)lua_newuserdata(serv->L, sizeof(pulsar_tcp_client));
	pulsar_setmeta(serv->L, MT_PULSAR_TCP_CLIENT);
	client->loop = serv->loop;
	client->fd = fd;
	client->active = false;
	client->disconnected = false;
	ev_io_init(&client->w_read, tcp_client_read_cb, fd, EV_READ);
	ev_io_init(&client->w_send, tcp_client_send_cb, fd, EV_WRITE);

	client->read_wait_len = 0;
	client->read_wait_until = NULL;
	client->read_buf_pos = 0;
	client->read_buf_len = DEFAULT_BUFFER_SIZE;
	client->read_buf = malloc(client->read_buf_len * sizeof(char));

	client->send_buf_pos = 0;
	client->send_buf_len = 0;
	client->send_buf = NULL;
	client->send_buf_chain = NULL;

	lua_State *L = lua_newthread(serv->L);
	lua_pushvalue(serv->L, -1);
	client->co_ref = luaL_ref(serv->L, LUA_REGISTRYINDEX);
	lua_xmove(serv->L, L, 3);
	pulsar_client_resume(client, L, 2);
}


/**************************************************************************************
 ** Timers
 **************************************************************************************/
static void pulsar_timer_resume(pulsar_timer *timer, lua_State *L, int nargs) {
	int ret = lua_resume(L, nargs);
	// More to do
	if (ret == LUA_YIELD) return;

	// Finished, end the timer
	if (!ret) {
		ev_timer_stop(timer->loop->loop, &timer->w_timeout);
		timer->active = false;
		return;
	}

	if (ret == LUA_ERRRUN) {
		printf("Error while running timer's coroutine: %s\n", lua_tostring(L, -1));
		traceback(L);

		ev_timer_stop(timer->loop->loop, &timer->w_timeout);
		timer->active = false;
		return;
	}
}

static int pulsar_timer_close(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	ev_timer_stop(timer->loop->loop, &timer->w_timeout);
	timer->active = false;
	luaL_unref(L, LUA_REGISTRYINDEX, timer->co_ref);
	return 0;
}
static int pulsar_timer_start(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	ev_timer_start(timer->loop->loop, &timer->w_timeout);
	timer->active = true;
	return 0;
}
static int pulsar_timer_stop(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	ev_timer_stop(timer->loop->loop, &timer->w_timeout);
	timer->active = false;
	return 0;
}
static int pulsar_timer_next(lua_State *L) {
	pulsar_timer *timer = (pulsar_timer *)luaL_checkudata (L, 1, MT_PULSAR_TIMER);
	return lua_yield(L, 0);
}

static void timer_cb(struct ev_loop *loop, ev_timer *_watcher, int revents) {
	if (EV_ERROR & revents) return;

	pulsar_timer *timer = (pulsar_timer *)_watcher;

	if (timer->first_run) {
		timer->first_run = false;
		pulsar_timer_resume(timer, timer->L, 1);
	} else {
		pulsar_timer_resume(timer, timer->L, 0);
	}
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
		ev_idle_stop(idle->loop->loop, &idle->w_timeout);
		idle->active = false;
		return;
	}

	if (ret == LUA_ERRRUN) {
		printf("Error while running idle's coroutine: %s\n", lua_tostring(L, -1));
		traceback(L);

		ev_idle_stop(idle->loop->loop, &idle->w_timeout);
		idle->active = false;
		return;
	}
}

static int pulsar_idle_close(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	ev_idle_stop(idle->loop->loop, &idle->w_timeout);
	idle->active = false;
	luaL_unref(L, LUA_REGISTRYINDEX, idle->co_ref);
	return 0;
}
static int pulsar_idle_start(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	ev_idle_start(idle->loop->loop, &idle->w_timeout);
	idle->active = true;
	return 0;
}
static int pulsar_idle_stop(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	ev_idle_stop(idle->loop->loop, &idle->w_timeout);
	idle->active = false;
	return 0;
}
static int pulsar_idle_next(lua_State *L) {
	pulsar_idle *idle = (pulsar_idle *)luaL_checkudata (L, 1, MT_PULSAR_IDLE);
	return lua_yield(L, 0);
}

static void idle_cb(struct ev_loop *loop, ev_idle *_watcher, int revents) {
	if (EV_ERROR & revents) return;

	pulsar_idle *idle = (pulsar_idle *)_watcher;

	if (idle->first_run) {
		idle->first_run = false;
		pulsar_idle_resume(idle, idle->L, 1);
	} else {
		pulsar_idle_resume(idle, idle->L, 0);
	}
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
		ev_idle_stop(idle_worker->loop->loop, &idle_worker->w_timeout);
		idle_worker->active = false;
		return;
	}

	if (ret == LUA_ERRRUN) {
		printf("Error while running idle_worker's coroutine: %s\n", lua_tostring(L, -1));
		traceback(L);

		ev_idle_stop(idle_worker->loop->loop, &idle_worker->w_timeout);
		idle_worker->active = false;
		return;
	}
}

static int pulsar_idle_worker_close(lua_State *L) {
	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)luaL_checkudata (L, 1, MT_PULSAR_IDLE_WORKER);
	ev_idle_stop(idle_worker->loop->loop, &idle_worker->w_timeout);
	idle_worker->active = false;
	while (idle_worker->chain) {
		pulsar_idle_worker_chain *chain = idle_worker->chain;
		idle_worker->chain = idle_worker->chain->next;

		free(chain);
	}
	return 0;
}
static int pulsar_idle_worker_split(lua_State *L) {
	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)luaL_checkudata (L, 1, MT_PULSAR_IDLE_WORKER);
	ev_idle_start(idle_worker->loop->loop, &idle_worker->w_timeout);
	idle_worker->active = true;

	pulsar_idle_worker_chain *chain = malloc(sizeof(pulsar_idle_worker_chain));
	lua_pushthread(L); chain->L = lua_tothread(L, -1); lua_pop(L, 1);
	chain->next = NULL;

	if (!idle_worker->chain) idle_worker->chain = chain;
	else {
		pulsar_idle_worker_chain *tail = idle_worker->chain;
		while (tail->next) tail = tail->next;
		tail->next = chain;
	}

	return lua_yield(L, 0);
}

static void idle_worker_cb(struct ev_loop *loop, ev_idle *_watcher, int revents) {
	if (EV_ERROR & revents) return;

	pulsar_idle_worker *idle_worker = (pulsar_idle_worker *)_watcher;
	if (!idle_worker->chain) {
		ev_idle_stop(idle_worker->loop->loop, &idle_worker->w_timeout);
		idle_worker->active = false;
		return;
	}

	pulsar_idle_worker_chain *chain = idle_worker->chain;
	idle_worker->chain = idle_worker->chain->next;
	lua_State *rL = chain->L;
	free(chain);

	pulsar_idle_worker_resume(idle_worker, rL, 0);
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
		loop->loop = ev_default_loop(0);
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
	loop->loop = ev_loop_new(EVFLAG_AUTO);
	return 1;
}

static int pulsar_loop_close(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	ev_loop_destroy(loop->loop);
	return 0;
}

static int pulsar_loop_run(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	ev_run(loop->loop, 0);
	return 0;
}

static int pulsar_tcp_server_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	const char *address = luaL_checkstring(L, 2);
	int port = luaL_checknumber(L, 3);
	if (!lua_isfunction(L, 4)) { lua_pushstring(L, "argument 3 is not a function"); lua_error(L); return 0; }

	int fd;
	struct sockaddr_in addr;
	int addr_len = sizeof(addr);

	// Create server socket
	if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	{
		lua_pushnil(L);
		lua_pushstring(L, "socket error when creating server");
		return 2;
	}

	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
		lua_pushnil(L);
		lua_pushstring(L, "socket error when resolving bind address");
		return 2;
	}

	// Bind socket to address
	if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) != 0)
	{
		lua_pushnil(L);
		lua_pushstring(L, "socket error when binding server");
		return 2;
	}

	// Start listing on the socket
	if (listen(fd, 50) < 0)
	{
		lua_pushnil(L);
		lua_pushstring(L, "socket error when listening server");
		return 2;
	}

	if (!set_non_blocking(fd)) {
		lua_pushnil(L);
		lua_pushstring(L, "socket error when setting server non blocking");
		return 2;
	}

	lua_pushvalue(L, 4);
	int fctref = luaL_ref(L, LUA_REGISTRYINDEX);

	// Initialize and start a watcher to accepts client requests
	pulsar_tcp_server *serv = (pulsar_tcp_server*)lua_newuserdata(L, sizeof(pulsar_tcp_server));
	pulsar_setmeta(L, MT_PULSAR_TCP_SERVER);
	serv->L = L;
	serv->loop = loop;
	serv->fd = fd;
	serv->active = false;
	serv->client_fct_ref = fctref;

	ev_io_init(&serv->w_accept, tcp_server_accept_cb, fd, EV_READ);
	return 1;
}

static int pulsar_tcp_client_new(lua_State *L)
{
	pulsar_loop *loop = (pulsar_loop *)luaL_checkudata (L, 1, MT_PULSAR_LOOP);
	const char *address = luaL_checkstring(L, 2);
	int port = luaL_checknumber(L, 3);

	// Create server socket
	int fd;
	if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	{
		lua_pushnil(L);
		lua_pushstring(L, "socket error when creating client");
		return 2;
	}

	struct sockaddr_in addr;
	int addr_len = sizeof(addr);
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
		lua_pushnil(L);
		lua_pushstring(L, "socket error when resolving connect address");
		return 2;
	}

	if (connect(fd, (struct sockaddr *)&addr, addr_len)) return 0;
	if (!set_non_blocking(fd)) return 0;

	// Initialize and start watcher to read client requests
	pulsar_tcp_client *client = (pulsar_tcp_client*)lua_newuserdata(L, sizeof(pulsar_tcp_client));
	pulsar_setmeta(L, MT_PULSAR_TCP_CLIENT);
	client->loop = loop;
	client->fd = fd;
	client->active = false;
	client->disconnected = false;
	ev_io_init(&client->w_read, tcp_client_read_cb, fd, EV_READ);
	ev_io_init(&client->w_send, tcp_client_send_cb, fd, EV_WRITE);

	client->read_wait_len = 0;
	client->read_wait_until = NULL;
	client->read_buf_pos = 0;
	client->read_buf_len = DEFAULT_BUFFER_SIZE;
	client->read_buf = malloc(client->read_buf_len * sizeof(char));

	client->send_buf_pos = 0;
	client->send_buf_len = 0;
	client->send_buf = NULL;
	client->send_buf_chain = NULL;

	return 1;
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
	lua_pushvalue(L, -1);
	lua_pushvalue(L, 4);
	lua_pushvalue(L, 5);
	lua_xmove(L, timer->L, 3);

	timer->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	ev_timer_init(&timer->w_timeout, timer_cb, first, repeat);
	return 1;
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
	lua_pushvalue(L, -1);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_xmove(L, idle->L, 3);

	idle->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	ev_idle_init(&idle->w_timeout, idle_cb);
	return 1;
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

	ev_idle_init(&idle->w_timeout, idle_worker_cb);
	return 1;
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
	{"longTask", pulsar_idle_worker_new},
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
	{"split", pulsar_idle_worker_split},
	{"close", pulsar_idle_worker_close},
	{"__gc", pulsar_idle_worker_close},
	{NULL, NULL},
};
static const struct luaL_reg meth_pulsar_tcp_server[] =
{
	{"start", pulsar_tcp_server_start},
	{"stop", pulsar_tcp_server_stop},
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

	pulsar_createmeta(L, MT_PULSAR_LOOP, meth_pulsar_loop);
	pulsar_createmeta(L, MT_PULSAR_TIMER, meth_pulsar_timer);
	pulsar_createmeta(L, MT_PULSAR_IDLE, meth_pulsar_idle);
	pulsar_createmeta(L, MT_PULSAR_IDLE_WORKER, meth_pulsar_idle_worker);
	pulsar_createmeta(L, MT_PULSAR_TCP_SERVER, meth_pulsar_tcp_server);
	pulsar_createmeta(L, MT_PULSAR_TCP_CLIENT, meth_pulsar_tcp_client);

	luaL_openlib(L, "pulsar", pulsarlib, 0);
	set_info(L);
	return 1;
}
