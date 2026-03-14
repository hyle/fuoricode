CC = gcc
CPPFLAGS += -DVERSION=\"$(VERSION)\"
CFLAGS = -Wall -Wextra -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls \
         -Wstrict-prototypes -Wold-style-definition -std=c99 -O2 -D_POSIX_C_SOURCE=200809L
TARGET = fuori
SOURCES = main.c collect.c render.c git_paths.c ignore.c
TEST_TARGET = test_ignore
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
VERSION ?= dev

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $(TARGET) $(SOURCES)

$(TEST_TARGET): test_ignore.c ignore.c ignore.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $(TEST_TARGET) test_ignore.c ignore.c

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET)
	BIN=./$(TARGET) sh ./test_cli.sh

clean:
	rm -f $(TARGET) $(TEST_TARGET)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

.PHONY: all clean install test
