CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -D_POSIX_C_SOURCE=200809L

THREADS = -pthread

SWITCH_OBJECTS = switch.o queue.o mac_table.o frame.o admin.o stats.o logger.o

SWCTL_OBJECTS = swctl.o stats.o frame.o

all: switch host swctl test

switch: $(SWITCH_OBJECTS)
	$(CC) $(CFLAGS) -o switch $(SWITCH_OBJECTS) $(THREADS)

host: host.o frame.o
	$(CC) $(CFLAGS) -o host host.o frame.o $(THREADS)

swctl: $(SWCTL_OBJECTS)
	$(CC) $(CFLAGS) -o swctl $(SWCTL_OBJECTS) $(THREADS)

test: test.o frame.o mac_table.o
	$(CC) $(CFLAGS) -o test test.o frame.o mac_table.o $(THREADS)

switch.o: switch.c switch.h frame.h queue.h mac_table.h admin.h logger.h
	$(CC) $(CFLAGS) -c switch.c

admin.o: admin.c switch.h frame.h queue.h mac_table.h admin.h logger.h
	$(CC) $(CFLAGS) -c admin.c

swctl.o: swctl.c admin.h frame.h
	$(CC) $(CFLAGS) -c swctl.c

stats.o: stats.c admin.h
	$(CC) $(CFLAGS) -c stats.c

logger.o: logger.c logger.h
	$(CC) $(CFLAGS) -c logger.c

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
	rm -f *.o switch host swctl test

.PHONY: all clean