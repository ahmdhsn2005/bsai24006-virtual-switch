CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -D_POSIX_C_SOURCE=200809L

THREADS = -pthread

SWITCH_OBJECTS = switch.o queue.o mac_table.o frame.o

all: switch host test

switch: $(SWITCH_OBJECTS)
	$(CC) $(CFLAGS) -o switch $(SWITCH_OBJECTS) $(THREADS)

host: host.o frame.o
	$(CC) $(CFLAGS) -o host host.o frame.o $(THREADS)

test: test.o frame.o mac_table.o
	$(CC) $(CFLAGS) -o test test.o frame.o mac_table.o $(THREADS)

switch.o: switch.c switch.h frame.h queue.h mac_table.h
	$(CC) $(CFLAGS) -c switch.c

queue.o: queue.c queue.h frame.h
	$(CC) $(CFLAGS) -c queue.c

mac_table.o: mac_table.c mac_table.h
	$(CC) $(CFLAGS) -c mac_table.c

frame.o: frame.c frame.h
	$(CC) $(CFLAGS) -c frame.c

host.o: host.c frame.h
	$(CC) $(CFLAGS) -c host.c

test.o: test.c frame.h mac_table.h
	$(CC) $(CFLAGS) -c test.c

clean:
	rm -f *.o switch host test

.PHONY: all clean