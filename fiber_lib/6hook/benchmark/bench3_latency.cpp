// Bench 3: keep-alive HTTP 延迟分布。
//
// 一个 poll 驱动的客户端管理所有连接，避免“一个连接一个客户端线程”污染高并发结果。
// concurrency 表示同时存在且每条最多一个在途请求的连接数。

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kRoundsPerConnection = 100;
constexpr int kPacketSize = 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(1);
constexpr int kPollTimeoutMs = 10;

const char* kRequestHeader =
    "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1024\r\n\r\n";

struct Stats {
    int planned = 0;
    int attempted = 0;
    int completed = 0;
    int timed_out = 0;
    int connect_failed = 0;
    int io_failed = 0;
    std::vector<uint64_t> latencies_us;
};

struct Connection {
    int fd = -1;
    int completed_rounds = 0;
    size_t request_sent = 0;
    size_t response_size = 0;
    std::chrono::steady_clock::time_point started;
    char response[4096];
    bool active = false;
};

size_t findHeaderEnd(const char* buffer, size_t length)
{
    for (size_t i = 0; i + 3 < length; ++i)
    {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n')
        {
            return i + 4;
        }
    }
    return 0;
}

size_t parseContentLength(const char* buffer, size_t header_end)
{
    static constexpr char kKey[] = "Content-Length:";
    for (size_t i = 0; i + sizeof(kKey) - 1 <= header_end; ++i)
    {
        if (std::memcmp(buffer + i, kKey, sizeof(kKey) - 1) != 0)
        {
            continue;
        }
        size_t pos = i + sizeof(kKey) - 1;
        while (pos < header_end && (buffer[pos] == ' ' || buffer[pos] == '\t'))
        {
            ++pos;
        }
        size_t value = 0;
        while (pos < header_end && buffer[pos] >= '0' && buffer[pos] <= '9')
        {
            value = value * 10 + static_cast<size_t>(buffer[pos] - '0');
            ++pos;
        }
        return value;
    }
    return 0;
}

int connectTo(const char* ip, int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    timeval timeout{};
    timeout.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

bool setNonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void closeConnection(Connection& connection)
{
    if (connection.fd >= 0)
    {
        close(connection.fd);
        connection.fd = -1;
    }
    connection.active = false;
}

void failConnection(Connection& connection, Stats& stats, bool timeout)
{
    if (!connection.active)
    {
        return;
    }
    if (timeout)
    {
        ++stats.timed_out;
    }
    else
    {
        ++stats.io_failed;
    }
    closeConnection(connection);
}

void startRequest(Connection& connection, Stats& stats)
{
    connection.request_sent = 0;
    connection.response_size = 0;
    connection.started = std::chrono::steady_clock::now();
    ++stats.attempted;
}

void handleWritable(Connection& connection, const std::string& request, Stats& stats)
{
    while (connection.request_sent < request.size())
    {
        const ssize_t written = send(connection.fd, request.data() + connection.request_sent,
                                     request.size() - connection.request_sent, 0);
        if (written > 0)
        {
            connection.request_sent += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        failConnection(connection, stats, false);
        return;
    }
}

bool responseComplete(const Connection& connection)
{
    const size_t header_end = findHeaderEnd(connection.response, connection.response_size);
    if (header_end == 0)
    {
        return false;
    }
    return connection.response_size >= header_end +
        parseContentLength(connection.response, header_end);
}

void handleReadable(Connection& connection, Stats& stats)
{
    while (connection.response_size < sizeof(connection.response))
    {
        const ssize_t received = recv(connection.fd, connection.response + connection.response_size,
                                      sizeof(connection.response) - connection.response_size, 0);
        if (received > 0)
        {
            connection.response_size += static_cast<size_t>(received);
            if (responseComplete(connection))
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - connection.started).count();
                stats.latencies_us.push_back(static_cast<uint64_t>(elapsed));
                ++stats.completed;
                ++connection.completed_rounds;
                if (connection.completed_rounds == kRoundsPerConnection)
                {
                    closeConnection(connection);
                }
                else
                {
                    startRequest(connection, stats);
                }
                return;
            }
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        failConnection(connection, stats, false);
        return;
    }
    failConnection(connection, stats, false);
}

void printPercentiles(std::vector<uint64_t>& latencies)
{
    if (latencies.empty())
    {
        std::cout << "  no successful requests";
        return;
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&latencies](double percentage) {
        size_t index = static_cast<size_t>(latencies.size() * percentage / 100.0);
        if (index >= latencies.size())
        {
            index = latencies.size() - 1;
        }
        return latencies[index];
    };
    std::cout << "  P50=" << percentile(50)
              << "us P90=" << percentile(90)
              << "us P99=" << percentile(99)
              << "us P99.9=" << percentile(99.9) << "us";
}

void benchPort(int port, const std::string& label)
{
    const std::vector<int> concurrencies = {4, 16, 64, 256, 1024};
    std::string request(kRequestHeader);
    request.append(kPacketSize, 'x');

    std::cout << "\n--- " << label << " (port " << port
              << "), HTTP 1KB RTT, " << kRoundsPerConnection
              << " rounds/connection, poll client ---\n";

    for (int concurrency : concurrencies)
    {
        Stats stats;
        stats.planned = concurrency * kRoundsPerConnection;
        stats.latencies_us.reserve(stats.planned);
        std::vector<Connection> connections;
        connections.reserve(concurrency);

        for (int i = 0; i < concurrency; ++i)
        {
            Connection connection;
            connection.fd = connectTo("127.0.0.1", port);
            if (connection.fd < 0 || !setNonblocking(connection.fd))
            {
                ++stats.connect_failed;
                closeConnection(connection);
                continue;
            }
            connection.active = true;
            startRequest(connection, stats);
            connections.push_back(std::move(connection));
        }

        while (true)
        {
            std::vector<pollfd> poll_fds;
            std::vector<size_t> indexes;
            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < connections.size(); ++i)
            {
                Connection& connection = connections[i];
                if (!connection.active)
                {
                    continue;
                }
                if (now - connection.started >= kRequestTimeout)
                {
                    failConnection(connection, stats, true);
                    continue;
                }
                short events = connection.request_sent < request.size() ? POLLOUT : POLLIN;
                poll_fds.push_back({connection.fd, events, 0});
                indexes.push_back(i);
            }
            if (poll_fds.empty())
            {
                break;
            }

            const int ready = poll(poll_fds.data(), poll_fds.size(), kPollTimeoutMs);
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                for (Connection& connection : connections)
                {
                    failConnection(connection, stats, false);
                }
                break;
            }
            for (size_t i = 0; i < poll_fds.size(); ++i)
            {
                Connection& connection = connections[indexes[i]];
                const short revents = poll_fds[i].revents;
                if (!connection.active || revents == 0)
                {
                    continue;
                }
                if (revents & (POLLERR | POLLHUP | POLLNVAL))
                {
                    failConnection(connection, stats, false);
                }
                else if (revents & POLLOUT)
                {
                    handleWritable(connection, request, stats);
                }
                else if (revents & POLLIN)
                {
                    handleReadable(connection, stats);
                }
            }
        }

        const int not_started = stats.planned - stats.attempted;
        const double success_rate = stats.planned == 0 ? 0.0 :
            100.0 * static_cast<double>(stats.completed) / stats.planned;
        std::cout << "  C=" << concurrency
                  << " planned=" << stats.planned
                  << " attempted=" << stats.attempted
                  << " completed=" << stats.completed
                  << " timed_out=" << stats.timed_out
                  << " connect_failed=" << stats.connect_failed
                  << " io_failed=" << stats.io_failed
                  << " not_started=" << not_started
                  << " success_rate=" << success_rate << '%';
        printPercentiles(stats.latencies_us);
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " <port_A> [port_B]\n";
        return 1;
    }

    const int port_a = atoi(argv[1]);
    const int port_b = argc >= 3 ? atoi(argv[2]) : -1;
    if (port_a > 0)
    {
        benchPort(port_a, "ServerA[POOL+BLOCK]");
    }
    if (port_b > 0)
    {
        benchPort(port_b, "ServerB[CORO+HOOK]");
    }
    return 0;
}
