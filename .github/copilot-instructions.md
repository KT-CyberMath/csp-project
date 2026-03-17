# AI Coding Agent Instructions for CSP Project

## Project Overview

This is a **Client-Server Protocol (CSP)** implementation in C providing a remote file management system. The server maintains user sessions with a login/home directory model and handles file/directory operations. The client connects via TCP socket and sends text commands.

**Architecture:**
- `server/server.c`: TCP server on port 8080, manages users & file operations
- `client/client.c`: Interactive TCP client sending commands
- `build/`: Compiled binaries (client, server)
- Root filesystem: `/home/ubuntu/csp_root/` contains user home directories

## Critical Knowledge

### Session & State Management
- Users login via `login <username>` command; server creates `$ROOT_DIR/$username` home directory
- **Global state:** `current_user[64]`, `current_dir[256]` track logged-in user per connection
- Commands require login (check `if (current_user[0] == '\0')`) except login/quit
- **Pattern:** Single client connection per server invocation; no multi-client handling yet

### Command Protocol (Text-based, no framing)
Server implements these commands, all case-sensitive:
- `login <username>` → Creates home dir, sets session state
- `pwd` → Returns current directory path
- `list` → Directory listing (DIR/FILE entries with sizes)
- `create <filename>` → Creates empty file (mode 0644)
- `mkdir <dirname>` → Creates directory (mode 0755)
- `quit` → Closes connection

**Response Format:** `OK: <msg>\n` or `ERR: <msg>\n` or specific output

### Important Implementation Details
- **Port hardcoded:** PORT=8080 in both files; argv[2] parameter ignored in server (bug: uses PORT not `port` variable)
- **Buffer sizes:** 1024-byte read buffer; 4096-byte reply buffer for list output
- **Security limitations:** No path validation (no `../` prevention); buffers use strncpy but vulnerable to overflow if pathnames exceed buffer sizes
- **String parsing:** Uses strcspn to strip newlines from input; strncat for safe concatenation

## Build & Run

```bash
gcc -o server/server server/server.c
gcc -o client/client client/client.c

# Terminal 1: Start server (optional: override root dir and port)
./server/server
# Or: ./server/server /custom/root/path 9000

# Terminal 2: Connect client
./client/client
# Then type commands: login alice, pwd, list, create file.txt, quit
```

## Developer Patterns & Conventions

1. **Error Handling:** Always check syscall returns (socket, bind, mkdir, etc.). Use perror() for system errors; custom error messages in protocol responses.
2. **File Path Construction:** Use snprintf with sizeof bounds; always validate errno (e.g., EEXIST for mkdir).
3. **Socket Communication:** Use read()/write() directly; fgets/printf only on client stdin/stdout.
4. **Testing:** Manual terminal-based testing (no automated test framework). Test expected workflows and edge cases (login twice, create duplicate file, etc.).

## Common Modifications

- **Multi-client support:** Replace single accept() loop with fork/select/epoll pattern
- **New commands:** Add `if (strncmp(buffer, "cmd", 3) == 0)` block before default case; update protocol docs
- **Persistence:** Add file I/O to serialize/restore user state across server restarts
- **Path validation:** Add strstr checks to block `..` traversal

## Known Limitations
- Single connection only; no concurrency
- No authentication (login accepts any username)
- No encryption (plaintext TCP)
- PORT hardcoded despite argv[2] parameter
- No buffer overflow protection for long filenames
