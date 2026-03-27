CC = gcc
COPS = -D_GNU_SOURCE -g

all: milestone1

milestone1: milestone1.c
	$(CC) $(COPS) -o milestone1 milestone1.c -lm
clean:
	rm -f milestone1
