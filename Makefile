all : server

server: quacker.c
	gcc -pthread -lpthread -g -o server quacker.c

clean:
	rm -f server