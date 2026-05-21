#ifndef SOCKET_H
#define SOCKET_H

#include <string>
#include <cstddef>

class TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();

    bool connect(const std::string& host, int port);
    void close();
    bool send_bytes(const std::string& data);

    // Read up to max_len bytes into buffer
    // > 0 bytes read
    //   0 peer closed
    //  -1 error
    int receive_bytes(char* buf, size_t max_len);
    int get_fd() const { return sock_fd; }

private:
    int sock_fd;
    
    TcpSocket(const TcpSocket&);
    TcpSocket& operator=(const TcpSocket&);
};

#endif
