# Chatgroup Network Programming Project

A simple C-based client/server chat application for a Network Programming course.

The project runs on Linux, tested for Ubuntu 22.04.5 LTS, and uses:

- TCP sockets
- GTK for GUI
- SQLite for chat/file metadata storage
- A custom packet protocol using `PacketHeader`
- File upload/download support

## Project Files

```text
.
├── protocol.h
├── chat_database.h
├── chat_database.c
├── project_server.c
├── project_client.c
├── Makefile
├── files/              # created automatically for uploaded files
└── chat_history.db     # created automatically by the server
```

## Requirements

Install required packages:

```bash
sudo apt update
sudo apt install build-essential libgtk-3-dev libsqlite3-dev sqlite3
```

## Build

From the project folder:

```bash
make clean
make
```

Expected output files:

```text
server_app
client_app
```

## Run

### 1. Start the server

Open Terminal 1:

```bash
./server_app
```

The server GUI should open.
'

### 2. Start clients

Open Terminal 2:

```bash
./client_app
```

Enter a username, for example:

```text
Ahmed
```

Open more terminals to start more clients:

```bash
./client_app
```

Example users:

```text
Ahmed
Sara
Omar
```
