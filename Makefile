CC = gcc
CFLAGS = -Wall -Wextra -g -pthread $(shell pkg-config --cflags gtk+-3.0)
LIBS = $(shell pkg-config --libs gtk+-3.0) -lsqlite3

all: server client

server: project_server.c chat_database.c chat_database.h protocol.h
	$(CC) $(CFLAGS) project_server.c chat_database.c -o server_app $(LIBS)

client: project_client.c protocol.h
	$(CC) $(CFLAGS) project_client.c -o client_app $(LIBS)

clean:
	rm -f server_app client_app chat.db
	rm -rf files received_*
