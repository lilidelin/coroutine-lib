// ============================================================
// Bench 3: 延迟分布（不同并发下 P50/P90/P99/P99.9）
//
// 需要 bench2_echo_servers 已经起好（A:8081, B:8082）再跑它
// 用法：./bench3_latency 8081 8082
//
// 它自己开客户端线程，向 echo server 发 HTTP POST（1KB body），
// 等完整 HTTP 响应回来记延迟（RTT）。并发 4/16/64/256/1024 分别测。
//
// 注意：bench2 是 HTTP 服务器（返回固定 "Hello, World!"），不是裸 echo。
// 所以这里按 HTTP 协议收发（与服务端 serve_keepalive 对称地解析响应）。
// 阻塞组（A）并发 > 线程数时，其余连接会被饿死，表现为大量 timeout。
// ============================================================

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

static const int ROUNDS_PER_CONN = 100;
static const int PKT = 1024; // 请求 body 大小

// 请求：HTTP POST + 1KB body
static const char *REQ_HEADER =
    "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1024\r\n\r\n";

static int connect_to(const char *ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  // 1 秒超时：饿死的连接会在 recv/send 上超时返回，而不是永久卡死
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip, &addr.sin_addr);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}

// ---- 客户端侧的 HTTP 响应解析（与服务端 serve_keepalive 对称） ----
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

// 发一个请求 + 读完整响应，返回 RTT（微秒）。失败/超时返回 false。
static bool do_round(int fd, uint64_t *us_out) {
  char req[PKT + 128];
  size_t hl = strlen(REQ_HEADER);
  memcpy(req, REQ_HEADER, hl);
  memset(req + hl, 'x', PKT);
  size_t total = hl + PKT;

  auto t0 = std::chrono::high_resolution_clock::now();

  size_t sent = 0;
  while (sent < total) {
    ssize_t n = send(fd, req + sent, total - sent, 0);
    if (n <= 0)
      return false;
    sent += (size_t)n;
  }

  char buf[4096];
  size_t len = 0;
  for (;;) {
    ssize_t n = recv(fd, buf + len, sizeof(buf) - len, 0);
    if (n <= 0)
      return false; // 超时 / EOF / 出错
    len += (size_t)n;
    size_t he = find_header_end(buf, len);
    if (he) {
      size_t cl = parse_content_length(buf, he);
      if (len >= he + cl)
        break; // 读满完整响应
    }
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  *us_out = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        t1 - t0)
                .count();
  return true;
}

static void bench_port(int port, const std::string &label) {
  std::vector<int> conccurencies = {4, 16, 64, 256, 1024};

  std::cout << "\n--- " << label << " (port " << port << ")  HTTP 1KB RTT, "
            << ROUNDS_PER_CONN << " rounds/conn ---\n";

  for (int c : conccurencies) {
    std::vector<int> fds(c, -1);
    bool ok = true;
    for (int i = 0; i < c; ++i) {
      fds[i] = connect_to("127.0.0.1", port);
      if (fds[i] < 0) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      std::cout << "  C=" << c << " connect fail, skip.\n";
      for (int fd : fds)
        if (fd >= 0)
          close(fd);
      continue;
    }

    std::vector<uint64_t> latencies;
    latencies.reserve(c * ROUNDS_PER_CONN);
    std::atomic<int> completed{0};
    std::atomic<int> timedout{0};
    std::mutex merge_mtx;

    auto worker = [&](int fd, int rounds) {
      std::vector<uint64_t> local;
      local.reserve(rounds);
      int done = 0;
      for (int r = 0; r < rounds; ++r) {
        uint64_t us = 0;
        if (do_round(fd, &us)) {
          local.push_back(us);
          ++done;
        } else {
          break; // 超时/出错：这条连接被饿死，不再继续
        }
      }
      completed.fetch_add(done);
      timedout.fetch_add(rounds - done);
      std::lock_guard<std::mutex> lk(merge_mtx);
      latencies.insert(latencies.end(), local.begin(), local.end());
    };

    std::vector<std::thread> ths;
    for (int i = 0; i < c; ++i)
      ths.emplace_back(worker, fds[i], ROUNDS_PER_CONN);
    for (auto &t : ths)
      t.join();
    for (int fd : fds)
      close(fd);

    if (latencies.empty()) {
      std::cout << "  C=" << c << "  no completed requests (all timeout).\n";
      continue;
    }
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    auto pct = [&](double p) -> uint64_t {
      size_t idx = (size_t)(n * p / 100.0);
      if (idx >= n)
        idx = n - 1;
      return latencies[idx];
    };
    int total_rounds = c * ROUNDS_PER_CONN;
    std::cout << "  C=" << c << "  P50=" << pct(50) << "us"
              << "  P90=" << pct(90) << "us"
              << "  P99=" << pct(99) << "us"
              << "  P99.9=" << pct(99.9) << "us"
              << "  completed=" << completed.load() << "/" << total_rounds
              << "  timeout=" << timedout.load() << "\n";
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " <port_A> [port_B]\n\n";
    std::cout << "  port_A: blocking echo server (bench2_echo_servers 8081)\n";
    std::cout << "  port_B: coroutine echo server (bench2_echo_servers 8082)\n\n";
    std::cout << "  Example (compare both):\n";
    std::cout << "    terminal1: ./bench2_echo_servers 0 &\n";
    std::cout << "    terminal2: ./bench3_latency 8081 8082\n";
    return 1;
  }

  int portA = atoi(argv[1]);
  int portB = argc >= 3 ? atoi(argv[2]) : -1;

  std::cout << "================================================\n";
  std::cout << "  Bench3 延迟分布对比 (HTTP 1KB RTT)\n";
  std::cout << "================================================\n";

  if (portA > 0)
    bench_port(portA, "ServerA[POOL+BLOCK]");
  if (portB > 0)
    bench_port(portB, "ServerB[CORO+HOOK  ]");
  return 0;
}
