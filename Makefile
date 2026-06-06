CC ?= cc
CFLAGS ?= -O2
CPPFLAGS += -Iinclude
WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow
THREAD_FLAGS = -pthread
COMMON_SOURCES = src/ami.c src/asterisk_compat.c src/channel_store.c \
	src/config.c src/connection.c

.PHONY: all clean test

all: astv

astv: src/main.c $(COMMON_SOURCES) include/ami.h include/channel_store.h \
	include/config.h include/asterisk_compat.h include/version.h \
	include/connection.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) -std=c11 \
		src/main.c $(COMMON_SOURCES) -o $@

tests/test: tests/test.c $(COMMON_SOURCES) include/ami.h \
	include/channel_store.h include/config.h include/asterisk_compat.h \
	include/connection.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) -std=c11 \
		tests/test.c $(COMMON_SOURCES) -o $@

test: tests/test
	./tests/test

clean:
	rm -f astv tests/test
