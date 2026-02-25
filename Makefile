CC = gcc
CFLAGS = -Wall -Wextra -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls \
         -Wstrict-prototypes -Wold-style-definition -std=c99 -O2 -D_POSIX_C_SOURCE=200809L
TARGET = fuori
SOURCES = fuori.c ignore.c
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

.PHONY: all clean install
