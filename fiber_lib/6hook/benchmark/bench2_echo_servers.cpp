// ============================================================
// Bench 2: Echo Server 吞吐（公平对比）
//
// 两组都：NUM_WORKERS 个工作线程，keep-alive 长连接 Echo（每个连接持续读请求→回包）
//
// A 组 8081 端口：阻塞线程池 + 真阻塞 recv/send（set_hook_enable=false +
// accept_f/recv）
//     一个 worker 被一条连接独占，并发连接数上限 = 线程数，其余连接排队饿死
// B 组 8082 端口：IOManager + hook，recv 拿不到数据时 yield，一个线程腾挪几千连接
//
// 压测：见同目录 echo.lua（配合 wrk）
//   wrk -t4 -c50   -d10s -s echo.lua http://127.0.0.1:8081/  # A组
//   wrk -t4 -c2000 -d10s -s echo.lua http://127.0.0.1:8082/  # B组
// ============================================================

#include "hook.h"
#include "fd_manager.h"
#include "ioscheduler.h"
#include <arpa/inet.h>
#include <condition_variable>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <cstring>

static const int NUM_WORKERS = 4;

// ★ HTTP 响应：wrk 要求合法 HTTP 响应才算成功请求
static const char *HTTP_RESPONSE = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "Connection: keep-alive\r\n"
                                   "\r\n"
                                   "Hello, World!";
static const size_t HTTP_RESPONSE_LEN = strlen(HTTP_RESPONSE);

// ============================================================
// 共用：keep-alive 请求解析
// 按 Content-Length 把 TCP 字节流切成完整请求，读满一个请求就回一次响应。
// ============================================================
static size_t find_header_end(const char *buf, size_t len) {
  for (size_t i = 0; i + 3 < len; ++i) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n')
      return i + 4;
  }
  return 0;
}

static size_t parse_content_length(const char *buf, size_t hdr_end) {
  static const char KEY[] = "Content-Length:";
  const size_t KLEN = sizeof(KEY) - 1; // 15
  for (size_t i = 0; i + KLEN <= hdr_end; ++i) {
    if (memcmp(buf + i, KEY, KLEN) == 0) {
      size_t p = i + KLEN;
      while (p < hdr_end && (buf[p] == ' ' || buf[p] == '\t'))
        ++p;
      size_t v = 0;
      while (p < hdr_end && buf[p] >= '0' && buf[p] <= '9') {
        v = v * 10 + (size_t)(buf[p] - '0');
        ++p;
      }
      return v;
    }
  }
  return 0;
}

// 处理一条 keep-alive 连接：读满一个请求就回一个响应，直到对端关闭（recv==0）或出错。
// 这里的 recv/send/close 都是 hooked 版本，行为由当前线程 t_hook_enable 决定：
//   - A 组 worker：hook 关 + fd 阻塞 → recv 真阻塞（线程被这条连接独占）
//   - B 组协程：  hook 开 + fd 非阻塞 → recv 拿不到数据就 yield（线程去跑别的协程）
static void serve_keepalive(int cfd) {
  char buf[4096];
  size_t len = 0;
  for (;;) {
    size_t hdr_end = find_header_end(buf, len);
    size_t content_len = hdr_end ? parse_content_length(buf, hdr_end) : 0;
    if (hdr_end && len >= hdr_end + content_len) {
      size_t total = hdr_end + content_len;
      size_t sent = 0;
      while (sent < HTTP_RESPONSE_LEN) {
        ssize_t w = send(cfd, HTTP_RESPONSE + sent, HTTP_RESPONSE_LEN - sent, 0);
        if (w <= 0) {
          close(cfd);
          return;
        }
        sent += w;
      }
      memmove(buf, buf + total, len - total);
      len -= total;
      continue;
    }
    if (len == sizeof(buf)) { // 请求过大，兜底关闭
      close(cfd);
      return;
    }
    ssize_t n = recv(cfd, buf + len, sizeof(buf) - len, 0);
    if (n <= 0) { // 0=对端关闭，<0=出错
      close(cfd);
      return;
    }
    len += n;
  }
}

// ============================================================
// 共用：创建 listen socket
// ============================================================
static int create_listen(int port, bool use_hook) {
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    perror("socket");
    exit(1);
  }
  if (use_hook) {
    // ★ 登记进 FdMgr：协程里的 accept 才能走 hook 的 yield-on-EAGAIN 路径。
    //    FdCtx::init() 会把 socket 设为 OS 非阻塞（sysNonblock），无需手动 O_NONBLOCK。
    sylar::FdMgr::GetInstance()->get(lfd, true);
  }
  int yes = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    exit(1);
  }
  if (listen(lfd, 65535) < 0) {
    perror("listen");
    exit(1);
  }
  return lfd;
}

// ============================================================
// A 组：线程池 + 真阻塞 recv/send
// ============================================================
struct BlockingPool {
  std::deque<int> q;
  std::mutex mtx;
  std::condition_variable cv;
  std::vector<std::thread> workers;
  bool stopped = false;

  explicit BlockingPool(int n) {
    for (int i = 0; i < n; ++i) {
      workers.emplace_back([this] {
        sylar::set_hook_enable(false); // ★ 关键：关 hook，真阻塞
        while (true) {
          int cfd = -1;
          {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [this] { return stopped || !q.empty(); });
            if (stopped && q.empty())
              return;
            cfd = q.front();
            q.pop_front();
          }
          // ★ keep-alive：独占这条连接，循环读到请求就回包，直到对端关闭
          serve_keepalive(cfd);
        }
      });
    }
  }
  void put(int fd) {
    {
      std::lock_guard<std::mutex> lk(mtx);
      q.push_back(fd);
    }
    cv.notify_one();
  }
  ~BlockingPool() {
    {
      std::lock_guard<std::mutex> lk(mtx);
      stopped = true;
    }
    cv.notify_all();
    for (auto &t : workers)
      if (t.joinable())
        t.join();
  }
};

static void run_blocking_server(int port, bool *running) {
  int lfd = create_listen(port, false); // false：阻塞 listen socket
  std::cout << "[POOL+BLOCK] echo server -> port " << port
            << " (blocking recv, " << NUM_WORKERS << " workers)\n";
  // ★ 用 shared_ptr，按值捕获进 lambda，避免函数返回后 pool 被析构导致
  // use-after-free
  auto pool = std::make_shared<BlockingPool>(NUM_WORKERS);
  std::thread([lfd, pool, running] {
    while (*running) {
      struct sockaddr_in cli;
      socklen_t len = sizeof(cli);
      // ★ 用真实 accept（accept_f）：返回阻塞 socket，不登记进 FdMgr
      int cfd = accept_f(lfd, (struct sockaddr *)&cli, &len);
      if (cfd < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      pool->put(cfd);
    }
  }).detach();
}

// ============================================================
// B 组：IOManager + hook
// ============================================================
static void coro_http_client(int cfd) {
  // ★ keep-alive：循环读到请求就回包；recv 拿不到数据时 hook 会让协程 yield
  serve_keepalive(cfd);
}

static void run_coro_hook_server(int port, bool *running,
                                 std::shared_ptr<sylar::IOManager> &out_iom) {
  int lfd = create_listen(port, true); // true：登记 FdMgr，accept/recv 走 hook
  std::cout << "[CORO+HOOK  ] echo server -> port " << port << " (hook recv, "
            << NUM_WORKERS << " workers)\n";

  auto iom =
      std::make_shared<sylar::IOManager>(NUM_WORKERS + 1, true, "echo-coro");
  out_iom = iom;
  iom->scheduleLock(std::function<void()>([lfd, iom, running] {
    while (*running) {
      struct sockaddr_in cli;
      socklen_t len = sizeof(cli);
      int cfd = accept(lfd, (struct sockaddr *)&cli, &len);
      if (cfd < 0) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        break;
      }
      iom->scheduleLock(
          std::function<void()>(std::bind(coro_http_client, cfd)));
    }
  }));
}

int main(int argc, char **argv) {
  // mode: 0=both  1=only A  2=only B
  int mode = 0;
  if (argc >= 2)
    mode = atoi(argv[1]);

  std::cout << "================================================\n";
  std::cout << "  Bench2 Echo Server 吞吐对比 (workers=" << NUM_WORKERS
            << ")\n";
  std::cout
      << "  A (blocked): port 8081  wrk -t4 -c50  -d10s -s echo.lua URL\n";
  std::cout
      << "  B (coroutine): port 8082  wrk -t4 -c2000 -d10s -s echo.lua URL\n";
  std::cout << "================================================\n";

  bool running = true;
  std::shared_ptr<sylar::IOManager> iom;

  if (mode == 0 || mode == 1)
    run_blocking_server(8081, &running);
  if (mode == 0 || mode == 2)
    run_coro_hook_server(8082, &running, iom);

  std::cout << "\nServers up. Ctrl+C to quit.\n";
  while (true)
    std::this_thread::sleep_for(std::chrono::seconds(60));
  running = false;
  return 0;
}
