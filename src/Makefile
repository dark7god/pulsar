LUA_PREFIX = /usr/local/
PREFIX	= /usr/local/
MODULE = pulsar
VERSION = 1.0.0

INSTALL_PREFIX = $(PREFIX)/lib/lua/5.1/

CC	= gcc
TARGET	= ../pulsar.so
OBJS	= pulsar.o
LIBS	=
#CFLAGS	= -I $(LUA_PREFIX)/include -fPIC -ggdb -DDMALLOC -DDMALLOC_FUNC_CHECK -Wall
#LDFLAGS	= -shared -fPIC -luv -ggdb -ldmalloc
CFLAGS	= -I $(LUA_PREFIX)/include -fPIC -ggdb -Wall
LDFLAGS	= -shared -fPIC -luv -ggdb

default: $(TARGET)


install: default
	install -d $(INSTALL_PREFIX)
	install $(TARGET) $(INSTALL_PREFIX)

clean:
	rm -rf $(OBJS) $(TARGET) $(MODULE)-$(VERSION)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@
