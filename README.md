README
Computer Systems and Programming – Winter Exam Project
A.Y. 2025–2026

Name: Konstantinos Tziakouris
Matricola: 2229757

============================================================
1. HOW TO COMPILE THE PROJECT
============================================================

The project includes a Makefile that compiles both the server and the client.

To compile the entire project:
    make

This command produces two executables:
    - server
    - client

To clean all compiled files:
    make clean

The project was developed and tested on Ubuntu 24.04.


============================================================
2. HOW TO START THE SERVER (REQUIRES SUDO)
============================================================

Syntax:
    sudo ./server <root_directory> [<IP>] [<port>]

Default values:
    IP   : 127.0.0.1
    Port : 8080

Root directory:
    - Chosen by the user
    - Created automatically if it does not exist
    - All user home directories and files are stored inside this directory

Examples:
    sudo ./server root
    sudo ./server root 127.0.0.1 8080

Server console:
    - The ONLY command accepted on the server console is:
        exit
    - When "exit" is typed:
        • The server shuts down
        • All connected clients are disconnected
        • All client processes are terminated

Note:
    sudo is required because the server creates system users and a shared group
    and temporarily drops privileges to enforce per-user file permissions.

============================================================
3. HOW TO START THE CLIENT
============================================================

Syntax:
    ./client <IP> <port>

Example:
    ./client 127.0.0.1 8080

Notes:
    - IP and port are required arguments.


============================================================
4. HELP AND COMMAND LIST
============================================================

Typing:
    help

prints the complete list of supported commands and their syntax.

The help command can be used at any time.

Unknown or invalid commands return:
    ERR: unknown command


============================================================
5. USER MANAGEMENT COMMANDS
============================================================

Create user:
    create_user <username> <permissions>

Login:
    login <username>

Logout (server-side only):
    quit

Notes:
    - No password is required
    - Each user has a private home directory under the server root
    - Users are sandboxed and cannot access other users’ files


============================================================
6. FILE AND DIRECTORY COMMANDS
============================================================

Change directory:
    cd <path>
    cd ..

List directory:
    list
    list <path>

Create file:
    create <path> <permissions>
    create -d <path> <permissions>


Change permissions:
    chmod <path> <octal_permissions>

Move:
    move <source> <destination>

Delete:
    delete <path>

All paths are validated and sandboxed inside the user’s home directory.

Notes:
    - Absolute paths are allowed only if they resolve inside the user’s home directory
    - For most filesystem commands, paths must resolve under the logged-in user home directory.
    - list also accepts absolute paths under the server root directory.

============================================================
7. READ AND WRITE COMMANDS
============================================================

Read file:
    read <path>
    read -offset=N <path>

Write file:
    write <path>
    write -offset=N <path>

Notes:
    - write <path> appends to the end of the file.
    - write -offset=N writes starting at byte offset N (does not truncate).
    - The client reads data from standard input and ends input with Ctrl-D on an empty line (press Enter then Ctrl-D). 
    - read sends file content from server to client
    - write reads input from standard input
    - Writing terminates with Ctrl+D (EOF)
    - Offset options allow partial reads and writes
    - File locking is enforced:
        • Multiple readers allowed
        • Writers are exclusive
        • Concurrent access returns ERR: file busy


============================================================
8. UPLOAD AND DOWNLOAD
============================================================

Upload file:
    upload <local_path> <server_path>
    upload -b <local_path> <server_path>

Download file:
    download <server_path> <local_path>
    download -b <server_path> <local_path>

Notes:
    - The "-b" option runs the operation in background
    - The client remains interactive
    - Background transfers use a separate socket
    - When the background operation finishes, a notification is printed


============================================================
9. FILE TRANSFER BETWEEN USERS
============================================================

Request file transfer:
    transfer_request <file> <destination_user>

Accept transfer:
    accept <directory> <request_id>

Reject transfer:
    reject <request_id>

Notes:
    - Transfers are mediated by the server
    - Users are notified in real time
    - Synchronization is handled via shared memory


============================================================
10. EXIT BEHAVIOR
============================================================

Client exit:
    exit

Notes:
    - The client cannot exit while background operations are running

Server exit:
    - Type "exit" in the server console


============================================================
11. DESIGN NOTES
============================================================

- TCP client-server architecture
- Fork-based concurrency
- Shared memory for cross-user notifications
- POSIX file locking for concurrent access
- Strict sandboxing and path validation
- Exact command matching to avoid ambiguous input
