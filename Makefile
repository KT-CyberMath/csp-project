CC=gcc
CFLAGS=-Wall -Wextra -Iinclude

all: client server

client: src/client/client.c src/client/commands.c src/client/transfers.c src/server/utils.c
	$(CC) $(CFLAGS) -o ./client src/client/client.c src/client/commands.c src/client/transfers.c src/server/utils.c

server: src/server/server.c src/server/commands.c src/server/session.c src/server/utils.c src/server/privilege.c src/server/path_utils.c
	$(CC) $(CFLAGS) -o ./server src/server/server.c src/server/commands.c src/server/session.c src/server/utils.c src/server/privilege.c src/server/path_utils.c

clean:
	rm -f ./client ./server

.PHONY: all clean
