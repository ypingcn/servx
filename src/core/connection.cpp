#include "connection.h"

#include <unistd.h>

#include <cstring>

#include "connection_pool.h"
#include "event_module.h"
#include "logger.h"
#include "timer.h"

namespace servx {

uint64_t Connection::count = 0;

Connection::Connection()
    : socket_fd(-1), read_event(this, false),
      write_event(this, true), listen(false),
      timeout(false), error(false) {
}

bool Connection::open(int fd, bool lst) {
    socket_fd = fd;
    conn_id = ++count;
    listen = lst;

    char sa[sizeof(sockaddr_in6)];
    socklen_t len = sizeof(sockaddr_in6);

    if (getsockname(fd, reinterpret_cast<sockaddr*>(sa), &len) == -1) {
        Logger::instance()->error("get peer sockaddr failed");
        return false;
    }
    local_addr.set_addr(reinterpret_cast<sockaddr*>(sa), len);

    return true;
}

void Connection::close() {
    if (socket_fd == -1) {
        Logger::instance()->warn("connection already closed");
        return;
    }

    if (read_event.is_timer()) {
        Timer::instance()->del_timer(&read_event);
    }

    if (write_event.is_timer()) {
        Timer::instance()->del_timer(&write_event);
    }

    del_connection(this);

    ctx = nullptr;
    recv_buf = nullptr;
    listen = 0;
    timeout = 0;
    error = 0;

    read_event.reset();
    write_event.reset();

    ConnectionPool::instance()->ret_connection(this);

    if (::close(socket_fd) == -1) {
        Logger::instance()->warn("close %d failed", socket_fd);
    }

    socket_fd = -1;
}

void Connection::init_recv_buf(int sz) {
    recv_buf = std::unique_ptr<Buffer>(new Buffer(sz));
}

int Connection::recv_data(Buffer* buf, uint32_t count) {
    int n = io_read(socket_fd, buf->get_last(), count);

    if (n > 0) {
        buf->move_last(n);
        if (static_cast<uint32_t>(n) < count) {
            read_event.set_ready(false);
        }
        return n;
    }

    if (n == 0) {
        read_event.set_eof(true);
    } else if (n == SERVX_ERROR) {
        error = 1;
    }

    read_event.set_ready(false);
    return n;
}

int Connection::send_data(Buffer* buf, uint32_t count) {
    if (count > buf->get_size()) {
        Logger::instance()->warn("[send data] count > size");
        count = buf->get_size();
    }

    int n = io_write(socket_fd, buf->get_pos(), count);

    if (n > 0) {
        buf->move_pos(n);
        if (static_cast<uint32_t>(n) < count) {
            read_event.set_ready(false);
        }
        return n;
    }

    if (n == SERVX_ERROR) {
        error = 1;
    }

    read_event.set_ready(false);
    return n;
}

int Connection::send_file(File* file) {
    if (!file->file_status()) {
        return SERVX_ERROR;
    }

    // TODO: speed limit

    long offset = file->get_offset();
    int n = io_sendfile(socket_fd, file->get_fd(), &offset,
        file->get_file_size());
    file->set_offset(offset);

    if (offset == file->get_file_size()) {
        return SERVX_OK;
    }

    if (n == SERVX_ERROR) {
        error = 1;
    }

    write_event.set_ready(false);
    return SERVX_AGAIN;
}

}
