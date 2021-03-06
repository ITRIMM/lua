#include <stdlib.h>
#include <errno.h>

//#define DEBUG 1

#if DEBUG
//#  include <syslog.h>
#  define dprint(s, ...) { fprintf(stderr, s, ##__VA_ARGS__); /*usleep(10000);*/ }
//#  define dprint(s, ...) { fprintf(stderr, s, ##__VA_ARGS__); usleep(100000); }
#else
#  define dprint(...) while(0) {}
#endif

#define lscheduler_c
#define LUA_LIB

#include "lscheduler.h"

#include "lauxlib.h"
#include "lualib.h"
#include "lcoco.h"
#include <string.h>

#define time_after(a,b)  ((long)(b) - (long)(a) < 0)
#define time_before(a,b) time_after(b, a)

#define MAXEVENTS 128

static size_t MS_PER_TICK;

#ifndef WIN32
static int epoll_fd;
#endif
static int exit_scheduler;
static struct timer_context timers;

#if DEBUG
static void dump_timeouts()
{
	/*struct timeout_ctx *item;
	dprint("timeouts: head:%p\n", &timeout_head);
	for (item = timeout_head.next; item != &timeout_head; item = item->next) {
		dprint("timer:%p, expire:%u, next:%p, data:%p\n", item, item->expire, item->next, item->data);
	}
	dprint("done\n");*/
}
#else
#define dump_timeouts() while(0) {}
#endif

#ifndef WIN32
static int init_epoll()
{
	int ret = epoll_create1(EPOLL_CLOEXEC);
	if (ret < 0)
		return ret;
	epoll_fd = ret;
	return 0;
}
#else
HANDLE iocp = 0;
static int init_iocp()
{
        DWORD err;
        iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!iocp) {
                return -(int)err;
        }
        return 0;
}
#endif

#if 0
static void insert_timeout(struct timeout_ctx* ctx)
{
	struct timeout_ctx *insert_before;
	
	dprint("insert_timeout: ctx:%p\n", ctx);
	
	for (insert_before = timeout_head.next; insert_before != &timeout_head; insert_before = insert_before->next) {
		if (time_before(ctx->expire, insert_before->expire))
			break;
	}

	ctx->next = insert_before;
	ctx->prev = insert_before->prev;
	insert_before->prev->next = ctx;
	insert_before->prev = ctx;
}

static void remove_timeout(struct timeout_ctx* ctx)
{
	dprint("remove_timeout ctx:%p\n", ctx);
	
	if (ctx == NULL || ctx->next == ctx)
		return;

	ctx->next->prev = ctx->prev;
	ctx->prev->next = ctx->next;
	ctx->prev = ctx;
	ctx->next = ctx;
}

static int get_timeout(void)
{
	clock_t now = times(0);
	struct timeout_ctx *first = timeout_head.next;
	if (first != &timeout_head) {
		long diff = (long)first->expire - (long)now;
		if (diff <= 0)
			return 0;
		return diff * MS_PER_TICK;
	}
	else
		return -1;
}
#endif

static void set_timeout(struct timer_list* timer, int timeout, void *data)
{
#ifndef WIN32
        unsigned long expire = (unsigned long)times(0) + ((unsigned long)timeout + (MS_PER_TICK-1)) / MS_PER_TICK;
#else
        unsigned long expire = (unsigned long)clock() + ((unsigned long)timeout + (MS_PER_TICK-1)) / MS_PER_TICK; // TODO
#endif
        dprint("set_timeout: timeout:%i, expire:%lu\n", timeout, expire);
        set_timer(timer, expire, 0, data);
        add_timer(&timers, timer);
}

static int beginwait(lua_State *L, int fd, int write, int timeout, struct wait_ctx *ctx)
{
  int status;

  dprint("luaL_wait: co:%p, ctx:%p, fd:%i, write:%i, timeout:%i\n", L, &ctx, fd, write, timeout);
  status = lua_pushthread(L);
  lua_pop(L, 1);
  if (status == 1) {
    errno = EINVAL;
    return -1;
  }
#ifndef WIN32
  if (timeout >= 0) {
          set_timeout(&ctx->timer, timeout, ctx);
          ctx->timeout = timeout;
  }
  else
          ctx->timeout = 0;
  ctx->L_thread = L;
  ctx->fd = fd;
  ctx->suspended = 0;
  ctx->cancel = 0;
  ctx->delfd = 0;

  if (fd >= 0) {
          int ret;
          ctx->event.events = write ? EPOLLOUT : EPOLLIN;
          ctx->event.data.ptr = ctx;
          ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ctx->event);
          if (ret) {
                  if (errno == ENOENT) {
                          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ctx->event)) {
                                  if (ctx->timeout) {
                                          ctx->timeout = 0;
                                          del_timer(&ctx->timer);
                                  }
                                  return -errno;
                          }
                          ctx->delfd = 1;
                  }
                  else {
                          if (ctx->timeout) {
                                  ctx->timeout = 0;
                                  del_timer(&ctx->timer);
                          }
                          return -errno;
                  }
          }
  }

  luaL_settls(L, &ctx);

  dprint("beginwait yield: co:%p\n", L);
  return lua_yield(L, 0);
#else
  ctx->timeout = timeout;
  if (timeout >= 0) {
          set_timeout(&ctx->timer, timeout, ctx);
          ctx->timeout = timeout;
  }
  else
          ctx->timeout = 0;

  ctx->L_thread = L;
  ctx->fd = fd;
  ctx->suspended = 0;
  ctx->cancel = 0;

  lua_pushlightuserdata(L, (void*)&ctx);
  lua_pushthread(L);
  lua_settable(L, LUA_REGISTRYINDEX);
  luaL_settls(L, &ctx);

  dprint("beginwait yield: co:%p\n", L);
  return lua_yield(L, 0);

#endif
}

static void endwait(lua_State *L, struct wait_ctx *ctx)
{
  luaL_settls(L, 0);

#ifndef _WIN32
  if (ctx->delfd) {
          struct epoll_event event = {0};
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, &event);
  }
#else
  (void)ctx;
#endif
}

LUALIB_API int l_newwaitctx(lua_State *L)
{
  struct wait_ctx *ctx = (struct wait_ctx*)lua_newuserdata(L, sizeof(struct wait_ctx));
  memset(ctx, 0, sizeof(struct wait_ctx));
  return 1;
}

LUALIB_API int l_beginwait(lua_State *L)
{
  int fd = luaL_optinteger(L, 1, -1);
  static const char* operations[] = {"read", "write", NULL};
  int op = luaL_checkoption(L, 2, "read", operations);
  int timeout = luaL_optinteger(L, 3, -1);
  struct wait_ctx *ctx = (struct wait_ctx*)lua_touserdata(L, 4);
  return beginwait(L, fd, op, timeout, ctx);
}

LUALIB_API int l_endwait(lua_State *L)
{
  struct wait_ctx *ctx = (struct wait_ctx*)lua_touserdata(L, 1);
  endwait(L, ctx);
  return 0;
}

#ifndef WIN32
LUALIB_API int luaL_wait(lua_State *L, int fd, int write, int timeout)
#else
LUALIB_API int luaL_wait(lua_State *L, int fd, int write, int timeout, struct wait_ctx *ctx)
#endif
{
        int status;
#ifndef WIN32
	struct wait_ctx ctx;
#endif

	dprint("luaL_wait: co:%p, ctx:%p, fd:%i, write:%i, timeout:%i\n", L, &ctx, fd, write, timeout);
    status = lua_pushthread(L);
    lua_pop(L, 1);
    if (status == 1) {
        errno = EINVAL;
        return -1;
    }
#ifndef WIN32
	if (timeout >= 0) {
		set_timeout(&ctx.timer, timeout, &ctx);
		ctx.timeout = timeout;
	}
	else
		ctx.timeout = 0;
	ctx.L_thread = L;
	ctx.fd = fd;
	ctx.suspended = 0;
	ctx.cancel = 0;

	if (fd >= 0) {
		int ret;
		ctx.event.events = write ? EPOLLOUT : EPOLLIN;
		ctx.event.data.ptr = &ctx;
		ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx.fd, &ctx.event);
		if (ret) {
			if (errno == ENOENT) {
				if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctx.fd, &ctx.event)) {
					if (ctx.timeout) {
						ctx.timeout = 0;
						del_timer(&ctx.timer);
					}
					return -errno;
				}
				ctx.delfd = 1;
			}
			else {
				if (ctx.timeout) {
					ctx.timeout = 0;
					del_timer(&ctx.timer);
				}
				return -errno;
			}
		}
	}
	
	lua_pushlightuserdata(L, (void*)&ctx);
	lua_pushthread(L);
	lua_settable(L, LUA_REGISTRYINDEX);
	luaL_settls(L, &ctx);

	dprint("yield: co:%p\n", L);
	lua_yield(L, 0);
	dprint("resumed: co:%p\n", L);
	status = luaL_checkinteger(L, -1);
	lua_pop(L, 1);
	
	luaL_settls(L, 0);
	lua_pushlightuserdata(L, (void*)&ctx);
	lua_pushnil(L);
	lua_settable(L, LUA_REGISTRYINDEX);

	if (ctx.delfd) {
		struct epoll_event event = {0};
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx.fd, &event);
	}

	return ctx.cancel ? -1 : status;
#else
	ctx->timeout = timeout;
	if (timeout >= 0) {
		set_timeout(&ctx->timer, timeout, ctx);
		ctx->timeout = timeout;
	}
	else
		ctx->timeout = 0;

	ctx->L_thread = L;
	ctx->fd = fd;
	ctx->suspended = 0;
	ctx->cancel = 0;

	lua_pushlightuserdata(L, (void*)&ctx);
	lua_pushthread(L);
	lua_settable(L, LUA_REGISTRYINDEX);
	luaL_settls(L, &ctx);

	dprint("yield: co:%p\n", L);
	lua_yield(L, 0);
	dprint("resumed: co:%p\n", L);
	status = luaL_checkinteger(L, -1);
	lua_pop(L, 1);

	luaL_settls(L, 0);
	lua_pushlightuserdata(L, (void*)&ctx);
	lua_pushnil(L);
	lua_settable(L, LUA_REGISTRYINDEX);

	return ctx->cancel ? -1 : status;
#endif
}

static int auxwait(lua_State *L, int fd, int write, int timeout)
{
#ifndef WIN32
	int ret = luaL_wait(L, fd, write, timeout);
#else
	int ret;
	struct wait_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ret = luaL_wait(L, fd, write, timeout, &ctx);
#endif
	if (ret < 0) {
		lua_pushnil(L);
		lua_pushstring(L, "error");
		lua_pushfstring(L, "wait error: %d", -ret);
		return 3;
	}
	else if (ret == 0) {
		lua_pushnil(L);
		lua_pushstring(L, "timeout");
		return 2;
	}

	lua_pushboolean(L, 1);
	return 1;
}

LUALIB_API int luaL_addfd(lua_State *L, int fd)
{
#ifndef WIN32
	struct epoll_event event = {0};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event)) {
		return -errno;
	}
	return 0;
#else
	CreateIoCompletionPort((HANDLE)(intptr_t)fd, iocp, (ULONG_PTR)7, 0);
	return 0;
#endif
}

LUALIB_API int luaL_delfd(lua_State *L, int fd)
{
#ifndef WIN32
	struct epoll_event event = {0};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event)) {
		return -errno;
	}
	return 0;
#else
	return 0;
#endif
}

static void on_thread_error(lua_State *L, lua_State *co, const char* err)
{
#ifndef WIN32
        syslog(LOG_ERR, "SCHEDULER: thread[%p]: %s\n", co, err);
#endif
}

static int do_resume_thread(lua_State *L, lua_State *co, int nargs)
{
	int ret;
	dprint("do_resume_thread L:%p, co:%p, nargs:%i\n", L, co, nargs);
	lua_xmove(L, co, nargs);
	lua_setthis(co);
	dprint("do_resume_thread SP:%p\n", &ret);
	ret = lua_resume(co, nargs);
	dprint("do_resume_thread resume ret:%i\n", ret);
	lua_setthis(L);
	dprint("do_resume_thread 1\n");
	if (ret == LUA_YIELD) {
		dprint("do_resume_thread 2 top:%i\n", lua_gettop(co));
		//lua_pop(co, lua_gettop(co));
		dprint("do_resume_thread 3\n");
		return 0;
	}

	if (ret != 0) {
	        const char *error;
		dprint("do_resume_thread 4\n");
		lua_xmove(co, L, 1);
		on_thread_error(L, co, lua_tostring(L, -1));
	}

	return -1;
}

static int resume_thread(lua_State *L, struct wait_ctx *ctx, int status)
{
	lua_State *co = ctx->L_thread;
	dprint("resume_thread L:%p, co:%p, ctx:%p, status:%i, suspended:%i\n", 
	       L, co, ctx, status, ctx->suspended);
	if (ctx->suspended)
		return 0;
	if (ctx->timeout) {
		ctx->timeout = 0;
	  del_timer(&ctx->timer);
	}
	lua_pushinteger(L, status);
	return do_resume_thread(L, co, 1);
}

static int l_loop(lua_State *L)
{
        unsigned long now;
        unsigned long timeout;
        struct list_base expired;
        struct list_base *item, *tmp;
#ifndef WIN32
	struct epoll_event events[MAXEVENTS];
	int ret;

	while (1) {
		if (exit_scheduler) {
			lua_pushfstring(L, "scheduler terminated");
			return 1;
		}
		
		dprint("loop: gettop:%i\n", lua_gettop(L));
		now = (unsigned long)times(0);
		timeout = get_next_timeout(&timers, now) * MS_PER_TICK;
		dprint("loop: wait for events timeout:%lu\n", timeout);
		ret = epoll_wait(epoll_fd, events, MAXEVENTS, timeout);
		dprint("loop: wait ret:%i\n", ret);
		if (ret > 0) {
			int i;
			for (i = 0; i < ret; i++) {
				struct wait_ctx *ctx = events[i].data.ptr;
				int status = events[i].events & (EPOLLHUP | EPOLLERR) ? -1 : 1;
				if (resume_thread(L, ctx, status))
					lua_pop(L, 1);
			}
		}
		else if (ret < 0 && errno != EINTR) {
			lua_pushfstring(L, "loop error:%d\n", errno);
			return 1;
		}

		now = (unsigned long)times(0);
		//dprint("loop: now:%lu\n", now);
		if (get_next_timeout(&timers, now) == 0) {
                  list_init(&expired);
                  process_timers_ex(&timers, &expired, now);
                  list_for_each_safe(&expired, item, tmp) {
                          struct timer_list *timer = container_of(item, struct timer_list, list);
                          if (resume_thread(L, (struct wait_ctx*)timer->data, 0))
                                  lua_pop(L, 1);
                  }
		}
	}
	return 0;
#else
	DWORD ret;
	OVERLAPPED *ov;
	ULONG nevents;
	DWORD bytes;
	ULONG_PTR key;
	int err;
	BOOL r;
	struct wait_ctx *ctx;
	int status;

	while (1) {
		if (exit_scheduler) {
						lua_pushfstring(L, "scheduler terminated");
						return 1;
		}

		dprint("loop: gettop:%i\n", lua_gettop(L));
		now = (unsigned long)clock();
		timeout = get_next_timeout(&timers, now) * MS_PER_TICK;
		dprint("loop: wait for events timeout:%lu\n", timeout);
		while (1) {
						dprint("loop: 1\n");
						r = GetQueuedCompletionStatus(iocp, &bytes, &key, &ov, timeout);
						if (!r && (err = GetLastError()) == WAIT_TIMEOUT)
								break;
						if (ov == NULL)
								break;
						timeout = 0;
						ctx = (struct wait_ctx *)ov;
						dprint("loop: 2 ctx:%p, key:%p\n", ctx, key);
						if (!ctx->suspended) {
										dprint("loop: 3\n");
										status = r ? 1 : -1;
										dprint("loop: resume on io status:%i\n", status);
										if (resume_thread(L, ctx, status))
														lua_pop(L, 1);
										dprint("loop: resume on io done\n");
						}
						dprint("loop: 4\n");
						ov = 0;
		}

		dprint("loop: check timeouts\n");

		now = (unsigned long)clock();
		//dprint("loop: now:%lu\n", now);
		if (get_next_timeout(&timers, now) == 0) {
			list_init(&expired);
			process_timers_ex(&timers, &expired, now);
			list_for_each_safe(&expired, item, tmp) {
							struct timer_list *timer = container_of(item, struct timer_list, list);
							dprint("loop: resume on timeout status:0 ctx:%p co:%p\n",
										 (struct wait_ctx*)timer->data, ((struct wait_ctx*)timer->data)->L_thread);
							if (resume_thread(L, (struct wait_ctx*)timer->data, 0))
											lua_pop(L, 1);
							dprint("loop: resume on timeout done\n");
			}
		}
	}
	return 0;
#endif
}

static int l_wait(lua_State *L)
{
	int fd = luaL_optinteger(L, 1, -1);
	static const char* operations[] = {"read", "write", NULL};
	int op = luaL_checkoption(L, 2, "read", operations);
	int timeout = luaL_optinteger(L, 3, -1);
	return auxwait(L, fd, op, timeout);
}

static int l_msleep(lua_State *L)
{
	int timeout = luaL_optinteger(L, 1, -1);
	return auxwait(L, -1, 0, timeout);
}

static int l_run(lua_State *L)
{
	lua_State *co = lua_tothread(L, 1);
	luaL_argcheck(L, co, 1, "coroutine expected");
	if (do_resume_thread(L, co, lua_gettop(L) - 1)) {
		lua_pushnil(L);
		lua_pushvalue(L, -2);
		return 2;
	}
	else {
		lua_pushboolean(L, 1);
		return 1;
	}
}

static int l_addfd(lua_State *L)
{
	int fd = luaL_checkinteger(L, 1);
	int ret = luaL_addfd(L, fd);
	if (ret < 0) {
		lua_pushnil(L);
		lua_pushfstring(L, "addfd error:%d", -ret);
		return 2;
	}
	
	lua_pushboolean(L, 1);
	return 1;
}

static int l_delfd(lua_State *L)
{
	int fd = luaL_checkinteger(L, 1);
	int ret = luaL_delfd(L, fd);
	if (ret < 0) {
		lua_pushnil(L);
		lua_pushfstring(L, "delfd error:%d", -ret);
		return 2;
	}
	
	lua_pushboolean(L, 1);
	return 1;
}

static int suspend_thread(lua_State *L, lua_State *co)
{
	struct wait_ctx *ctx;
#ifndef WIN32
	struct epoll_event event = {0};
#endif

	ctx = (struct wait_ctx*)luaL_gettls(co);
	
	dprint("suspend_thread: co:%p, ctx:%p\n", co, ctx);

	if (ctx == NULL || ctx->suspended)
		return 0;

	ctx->suspended = 1;
	if (ctx->timeout) {
		dprint("suspend_thread: remove timeout:%p\n", &ctx->timer);
		ctx->timeout = 0;
		del_timer(&ctx->timer);
	}

#ifndef WIN32
	if (ctx->fd >= 0)
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, &event);
#endif

	return 1;
}

static int l_suspend_thread(lua_State *L)
{
	int ret;
	lua_State *co = lua_tothread(L, 1);
	luaL_argcheck(L, co, 1, "coroutine expected");
	ret = suspend_thread(L, co);
	lua_pushboolean(L, ret ? 1 : 0);
	return 1;
}

static int l_resume_thread(lua_State *L)
{
	struct wait_ctx *ctx;
	lua_State *co = lua_tothread(L, 1);
	luaL_argcheck(L, co, 1, "coroutine expected");
	ctx = (struct wait_ctx*)luaL_gettls(co);

	if (ctx == NULL || !ctx->suspended) {
		lua_pushboolean(L, 0);
		return 1;
	}

	ctx->suspended = 0;

	if (ctx->timeout)
		add_timer(&timers, &ctx->timer);

#ifndef WIN32
	if (ctx->fd >= 0) {
		if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctx->fd, &ctx->event)) {
			luaL_error(L, "epoll_clt error:%d", errno);
		}
	}
#endif
	
	lua_pushboolean(L, 1);
	return 1;
}

static int l_cancel_wait(lua_State *L)
{
	struct wait_ctx *ctx;
	lua_State *co = lua_tothread(L, 1);
	dprint("Cancel thread %p\n", co);
	luaL_argcheck(L, co, 1, "coroutine expected");
	ctx = (struct wait_ctx*)luaL_gettls(co);

	if (ctx == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}

	if (ctx->cancel) {
		lua_pushboolean(L, 1);
		return 1;
	}

	suspend_thread(L, co);

	// resume thread with 0 timeout
	ctx->timeout = 1;
	ctx->cancel = 1;
	ctx->suspended = 0;
	ctx->L_thread = co;
	set_timeout(&ctx->timer, 0, ctx);

	lua_pushboolean(L, 1);
	return 1;
}

static int l_exit(lua_State *L)
{
	exit_scheduler = 1;
	return 0;
}

static const luaL_Reg lib[] = {
	{"wait",   l_wait},
	{"cancel_wait", l_cancel_wait},
	{"msleep", l_msleep},
	{"loop",   l_loop},
	{"run",    l_run},
	{"addfd",  l_addfd},
	{"delfd",  l_delfd},
	{"suspend_thread", l_suspend_thread},
	{"resume_thread",  l_resume_thread},
	{"beginwait", l_beginwait},
	{"endwait", l_endwait},
	{"newwaitctx", l_newwaitctx},
	{"exit", l_exit},
  {NULL, NULL}
};

/* }====================================================== */

LUALIB_API int luaopen_scheduler (lua_State *L)
{
	lua_setthis(L);
	exit_scheduler = 0;
#ifndef WIN32
	MS_PER_TICK = 1000UL / sysconf(_SC_CLK_TCK);
	init_timers(&timers, (unsigned long)times(0));
	if (init_epoll())
                return 0;
#else
	MS_PER_TICK = 1000UL / CLOCKS_PER_SEC;
	init_timers(&timers, (unsigned long)clock());
	if (init_iocp())
	        return 0;
#endif
	luaL_register(L, LUA_SCHEDULERLIBNAME, lib);
	return 1;
}
