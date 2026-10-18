CC = gcc

CFLAGS = -O2 -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra

all: memory_access matrix threads

memory_access: memory_access.c
	$(CC) $(CFLAGS) $< -o $@

matrix: matrix.c
	$(CC) $(CFLAGS) $< -o $@ -lm

threads: threads.c
	$(CC) $(CFLAGS) -pthread $< -o $@

clean:
	rm -f memory_access matrix threads