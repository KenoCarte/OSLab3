CC = gcc
CFLAGS = -L../ -lmy_vm -m32 -lm

all: test

test: ../libmy_vm.a
	$(CC) test.c $(CFLAGS) -o test

multi_test: ../libmy_vm.a
	$(CC) multi_test.c $(CFLAGS) -pthread -o multi_test

example: ../libmy_vm.a
	$(CC) example.c $(CFLAGS) -o example

clean:
	rm -rf test multi_test example
