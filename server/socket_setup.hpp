#ifndef SOCKET_SETUP_HPP
#define SOCKET_SETUP_HPP

// Create, bind, and listen on a TCP socket.
// Returns a listening fd on success, or -1 on failure (and prints an error).
int create_listen_socket(int port, int backlog);

#endif

