CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -g
LDFLAGS =

SRCS = main.c lexer.c parser.c codegen.c
OBJS = $(SRCS:.c=.o)
TARGET = forge

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	ls -la $@
	@echo "forge: compiler binary: $$(wc -c < $@) bytes"

%.o: %.c forge.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	@for f in ../examples/*.forge; do \
		echo "=== testing $$f ==="; \
		./$(TARGET) "$$f" -o "/tmp/$$(basename $$f .forge)" 2>&1; \
		file "/tmp/$$(basename $$f .forge)"; \
	done

clean:
	rm -f $(OBJS) $(TARGET) a.out
