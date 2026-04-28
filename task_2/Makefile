CC = gcc
CFLAGS = -O2 -Wall -Wextra

all: file_lock

file_lock: file_lock.c
	$(CC) $(CFLAGS) -o file_lock file_lock.c

clean:
	rm -f file_lock