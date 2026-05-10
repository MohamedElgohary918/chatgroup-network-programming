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

## How to Use

### Public message

Type a normal message:

```text
Hello everyone
```

All connected users should receive it.

### Private message

Use this format:

```text
@username:message
```

Example:

```text
@Sara:Hello Sara
```

Only Sara should receive the message.

### Show uploaded files

Type:

```text
/files
```

The server sends the available uploaded file list.

### Download a file

Type:

```text
/get filename
```

Example:

```text
/get report.pdf
```

The downloaded file will be saved on the client side as:

```text
received_report.pdf
```

### Upload a file

Click the file button in the client GUI and choose a file.

The server saves uploaded files inside:

```text
files/
```

## Check Saved Chat History

Open the SQLite database:

```bash
sqlite3 chat_history.db
```

Show tables:

```sql
.tables
```

Show saved messages:

```sql
.headers on
.mode column
SELECT id, room, sender, message, is_private, target, timestamp FROM messages;
```

Show uploaded file records:

```sql
.headers on
.mode column
SELECT id, filename, uploader, filesize, room, timestamp FROM files;
```

Exit SQLite:

```sql
.exit
```

Or check messages directly from terminal:

```bash
sqlite3 chat_history.db "SELECT id, sender, message, is_private, target, timestamp FROM messages;"
```

## Suggested Test Scenario

Use this order for demo/testing:

```text
1. Start server.
2. Start Ahmed client.
3. Start Sara client.
4. Check online users list.
5. Ahmed sends public message.
6. Sara replies.
7. Ahmed sends private message to Sara using @Sara:message.
8. Check saved messages in SQLite.
9. Ahmed uploads a file.
10. Sara sends /files.
11. Sara sends /get filename.
12. Confirm the downloaded file exists.
13. Kick one user from the server GUI.
14. Shutdown server.
```

## Git Branch Workflow

Recommended branch for this fixed version:

```bash
git checkout -b fixed-protocol-sqlite-filetransfer
```

Add files:

```bash
git add .
```

Commit:

```bash
git commit -m "Add fixed protocol, SQLite storage, and file transfer baseline"
```

Push branch:

```bash
git push -u origin fixed-protocol-sqlite-filetransfer
```

Then open a Pull Request on GitHub.

## Notes

- The server must run before clients.
- Duplicate usernames should be rejected.
- Use small files for testing.
- Keep generated files such as `*.db`, `server_app`, `client_app`, and `files/` out of Git unless your instructor asks otherwise.

## References

- GTK recommends using `pkg-config` to compile GTK applications: https://docs.gtk.org/gtk3/compiling.html
- SQLite command-line shell documentation: https://sqlite.org/cli.html
- Git push documentation: https://git-scm.com/docs/git-push
