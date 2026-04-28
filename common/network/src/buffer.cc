#include "buffer.h"

#include <errno.h>
#include <sys/uio.h>

#include "SocketsOps.h"

namespace network {
    const char Buffer::kCRLF[] = "\r\n";

    const size_t Buffer::kCheapPrepend;
    const size_t Buffer::kInitialSize;

    ssize_t Buffer::readFd(int fd, int *savedErrno) {
        char extrabuf[1024 * 1024];
        struct iovec vec[2];
        const size_t writable = writableBytes();
        vec[0].iov_base = begin() + writerIndex_;
        vec[0].iov_len = writable;
        vec[1].iov_base = extrabuf;
        vec[1].iov_len = sizeof extrabuf;
        const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
        const ssize_t n = sockets::readv(fd, vec, iovcnt);
        if (n < 0) {
            *savedErrno = errno;
        } else if (n <= writable) {
            writerIndex_ += n;
        } else {
            writerIndex_ = buffer_.size();
            append(extrabuf, n - writable);
        }
        return n;
    }
}
