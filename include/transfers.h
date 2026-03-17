
#ifndef CLIENT_TRANSFERS_H
#define CLIENT_TRANSFERS_H

/* 
Client-side file transfer helpers
Used by both foreground and background (-b) transfers
*/

/* 
Upload local file to server
sock: connected socket
client_path: local source file
server_path: destination on server
returns 0 on success, -1 on error 
*/
int uploadFile_simple(int sock, const char *client_path, const char *server_path);

/* 
Download file from server
sock: connected socket
server_path: source on server
client_path: local destination file
returns 0 on success, -1 on error 
*/
int downloadFile_simple(int sock, const char *server_path, const char *client_path);

#endif

