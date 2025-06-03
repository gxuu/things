// Demonstrate how one would use co_await with epoll
// To compile
// g++ -o epoll_echo_server epoll_echo_server.cpp -std=c++23 -O0 -g
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>

#define PORT 8080

int create_and_bind_socket() {
  int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if(server_fd == -1) {
    perror("socket");
    return -1;
  }

  sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if(bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    perror("bind");
    close(server_fd);
    return -1;
  }

  if(listen(server_fd, SOMAXCONN) == -1) {
    perror("listen");
    close(server_fd);
    return -1;
  }

  return server_fd;
}

struct task {
  struct promise_type {
    std::exception_ptr excp;
    // Take notice here!
    auto initial_suspend() noexcept { return std::suspend_never{}; }
    auto final_suspend() noexcept { return std::suspend_always{}; }
    task get_return_object() { return {handle_t::from_promise(*this)}; }
    void unhandled_exception() { excp = std::current_exception(); }
  };

  using handle_t = std::coroutine_handle<promise_type>;
  handle_t handle;
};

struct CoroContext;

enum fdtype {
  ACCEPTOR,
  CONNECTION,
};

enum current_op_t {
  READING,
  WRITING,
};

struct userdata {
  fdtype       type;
  int          fd;
  int          result;
  sockaddr*    addr;
  socklen_t*   addrlen;
  char*        buf;
  size_t       size;
  current_op_t current_op;
};

// Think about these variables as maps: the key is fds, the value is epoll_event;
std::array<epoll_event, 1024> revents;
std::array<epoll_event, 1024> epoll_events;
std::array<task, 1024>        tasks;
std::array<userdata, 1024>    userdatas;

struct awaitable {
  CoroContext* ctx;
  userdata     data;

  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept { userdatas[data.fd] = data; }
  int  await_resume() { return userdatas[data.fd].result; }
};

struct CoroContext {
  int epfd = epoll_create1(0);

  void add_fd(int fd, int monitoring, bool acc) {
    epoll_event* event = &epoll_events[fd];
    event->events      = monitoring;
    event->data.u64    = fd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, event);
  }

  void add_task(task t, int id) { tasks[id] = t; }

  void run() {
    while(true) {
      int n = epoll_wait(epfd, revents.data(), 1024, -1);
      for(int ii = 0; ii < n; ++ii) {
        epoll_event& revent = revents[ii];
        userdata&    data   = userdatas[revent.data.u64];
        if((revent.events & EPOLLIN) && data.type == ACCEPTOR) {
          data.result = accept4(data.fd, data.addr, data.addrlen, SOCK_NONBLOCK);
        } else if((revent.events & EPOLLIN) && data.type == CONNECTION && data.current_op == READING) {
          data.result = read(data.fd, data.buf, data.size);
          epoll_events[data.fd].events |= EPOLLOUT;
          epoll_ctl(epfd, EPOLL_CTL_MOD, data.fd, &epoll_events[data.fd]);
          if(errno == EAGAIN) continue;
        } else if((revent.events & EPOLLOUT) && data.type == CONNECTION && data.current_op == WRITING) {
          data.result = write(data.fd, data.buf, data.size);
          epoll_events[data.fd].events &= ~EPOLLOUT;
          epoll_ctl(epfd, EPOLL_CTL_MOD, data.fd, &epoll_events[data.fd]);
        } else
          continue;
        tasks[data.fd].handle.resume();
      }
    }
  }
};

awaitable async_accept(CoroContext* ctx, int fd, sockaddr* addr, socklen_t* addrlen) {
  awaitable res;
  res.data.type    = ACCEPTOR;
  res.data.fd      = fd;
  res.data.addr    = addr;
  res.data.addrlen = addrlen;
  return res;
}

awaitable async_read(CoroContext* ctx, int fd, char* buf, size_t size) {
  awaitable res;
  res.data.type       = CONNECTION;
  res.data.fd         = fd;
  res.data.buf        = buf;
  res.data.size       = size;
  res.data.current_op = READING;
  return res;
}

awaitable async_write(CoroContext* ctx, int fd, char* buf, size_t size) {
  awaitable res;
  res.data.type       = CONNECTION;
  res.data.fd         = fd;
  res.data.buf        = buf;
  res.data.size       = size;
  res.data.current_op = WRITING;
  return res;
}

task echo_session(CoroContext* ctx, int fd) {
  char buf[9]{};
  while(true) {
    int bytes_read = co_await async_read(ctx, fd, buf, 8);
    if(!bytes_read) {
      printf("Connection [%d] has been closed by remote\n", fd);
      close(fd);
      continue;
    }
    buf[bytes_read] = 0;
    printf("fd = %d, bytes_read = %d, buf = %s\n", fd, bytes_read, buf);
    int bytes_write = co_await async_write(ctx, fd, buf, bytes_read);
    printf("bytes_write = %d\n", bytes_write);
  }
}

task accept_connection(CoroContext* ctx, int fd, sockaddr* addr, socklen_t* addrlen) {
  while(true) {
    int conn = co_await async_accept(ctx, fd, addr, addrlen);
    ctx->add_fd(conn, EPOLLIN | EPOLLOUT | EPOLLET, false);
    ctx->add_task(echo_session(ctx, conn), conn);
  }
}

int main() {
  CoroContext context;
  int         listen1 = create_and_bind_socket();

  sockaddr  addr;
  socklen_t addrlen;

  context.add_fd(listen1, EPOLLIN, true);
  context.add_task(accept_connection(&context, listen1, &addr, &addrlen), listen1);

  context.run();
}
