#include ../conf

CFLAGS= $(WARNINGS) $(DEFINES) $(INCLUDES)
DEFINES= -D_XOPEN_SOURCE=600 $(POSIX_SPAWN)
INCLUDES= $(LUA_INC)
WARNINGS= -W -Wall
LIBS= $(LUALIB)

T= ex.so
default: $(T)

OBJS= ex.o spawn.o $(EXTRA)
$(T): $(OBJS) $(EXTRA); $(CC) -shared -o $@ $(OBJS) $(LIBS)
libex.a: $(OBJS) $(EXTRA)
	$(AR) rcu $@ $(OBJS)
	$(RANLIB) $@
ex.o: ex.c spawn.h
spawn.o: spawn.c spawn.h
posix_spawn.o: posix_spawn.c posix_spawn.h

clean:; rm -f *.o ex.so ex.dll $(T)
