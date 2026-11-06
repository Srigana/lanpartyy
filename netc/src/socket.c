#include "netc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int netc_socket_open(uint16_t port)
{
    struct sockaddr_in addr;
    int flags;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void netc_socket_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

int netc_send(int fd, const struct sockaddr_in *to, const uint8_t *buf, size_t len)
{
    ssize_t n = sendto(fd, buf, len, 0, (const struct sockaddr *)to, sizeof(*to));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }
    return (int)n;
}

int netc_recv(int fd, struct sockaddr_in *from, uint8_t *buf, size_t cap)
{
    socklen_t addr_len = sizeof(*from);
    ssize_t n = recvfrom(fd, buf, cap, 0, (struct sockaddr *)from, &addr_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }
    return (int)n;
}
