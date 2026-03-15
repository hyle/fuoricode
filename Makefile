CC = gcc
CPPFLAGS += -DVERSION=\"$(VERSION)\"
CPPFLAGS += -Isrc
CFLAGS = -Wall -Wextra -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls \
         -Wstrict-prototypes -Wold-style-definition -std=c99 -O2 -D_POSIX_C_SOURCE=200809L
TARGET = fuori
TEST_CLI_TARGET = fuori-test
SOURCES = src/main.c src/collect.c src/render.c src/git_paths.c src/ignore.c src/options.c src/tree.c
TEST_TARGET = test_ignore
TREE_TEST_TARGET = test_tree
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
VERSION ?= dev

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $(TARGET) $(SOURCES)

$(TEST_CLI_TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DFUORI_TESTING -o $(TEST_CLI_TARGET) $(SOURCES)

$(TEST_TARGET): tests/test_ignore.c src/ignore.c src/ignore.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $(TEST_TARGET) tests/test_ignore.c src/ignore.c

$(TREE_TEST_TARGET): tests/test_tree.c src/tree.c src/tree.h src/collect.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $(TREE_TEST_TARGET) tests/test_tree.c src/tree.c

test: $(TARGET) $(TEST_CLI_TARGET) $(TEST_TARGET) $(TREE_TEST_TARGET)
	./$(TEST_TARGET)
	./$(TREE_TEST_TARGET)
	BIN=./$(TEST_CLI_TARGET) sh ./tests/test_cli.sh

clean:
	rm -f $(TARGET) $(TEST_CLI_TARGET) $(TEST_TARGET) $(TREE_TEST_TARGET)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

.PHONY: all clean install test
