CC     = gcc
CFLAGS = -O2 -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra

all: memory_access matrix threads false_sharing

memory_access: memory_access.c
	$(CC) $(CFLAGS) $< -o $@

matrix: matrix.c
	$(CC) $(CFLAGS) $< -o $@ -lm

threads: threads.c
	$(CC) $(CFLAGS) -pthread $< -o $@

false_sharing: false_sharing.c
	$(CC) $(CFLAGS) -pthread $< -o $@

clean:
	rm -f memory_access matrix threads false_sharing