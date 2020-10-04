#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <cstdlib>
#include <asio/awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::redirect_error;
using asio::use_awaitable;
using namespace std;

/**
 * Splits a string using the specified delimiter.
 */
vector<string> split_string(const string& input, const string& delimiter) {
    vector<string> result;

    size_t last = 0;
    size_t next;

    while ((next = input.find(delimiter, last)) != string::npos) {
        result.push_back(input.substr(last, next - last));
        last = next + 1;
    }

    if (last < input.size()) {
        result.push_back(input.substr(last));
    }

    return result;
}

/**
 * HttpConnect parse exceptions
 */
class InvalidHttpConnectRequest : public std::exception {
};

class UnsupportedHttpVerb : public InvalidHttpConnectRequest {
};

/**
 * Parses an HTTP CONNECT request, returns the target address.
 */
class HttpConnectRequest {
public:
    static HttpConnectRequest FromRequestString(const string& request_string) {
        vector<string> request_lines = split_string(request_string, "\r\n");
        cout << "number of lines: " << request_lines.size() << endl;

        if (request_lines.empty()) {
            throw InvalidHttpConnectRequest();
        }

        vector<string> connect_parameters = split_string(request_lines[0], " ");
        if (connect_parameters.size() < 3) { // [0] = CONNECT, [1] = host:port, [2] = HTTP/1.1
            throw InvalidHttpConnectRequest();
        }

        if (connect_parameters[0] != "CONNECT") {
            throw UnsupportedHttpVerb();
        }

        vector<string> target = split_string(connect_parameters[1], ":");
        const string DEFAULT_PORT = "80";
        if (target.size() == 1) {
            return HttpConnectRequest(target[0], DEFAULT_PORT);
        } else {
            return HttpConnectRequest(target[0], target[1]);
        }
    }

    HttpConnectRequest() = delete;

    string GetHost() const {
        return _host;
    }

    string GetPort() const {
        return _port;
    }

private:
    HttpConnectRequest(const string& host, const string& port)
    : _host(host), _port(port) {
    }

    string _host;
    string _port;
};

/**
 * HTTP Tunnel
 */
class HttpTunnel : public enable_shared_from_this<HttpTunnel> {

public:
    explicit HttpTunnel(tcp::socket accepting_socket, asio::io_context& io_context)
            : _accepting_socket(move(accepting_socket)),
              _io_context(io_context),
              _target_socket(io_context),
              _resolver(io_context) {
        cout << "Constructing tunnel" << endl;
    }

    void start() {
        cout << "Starting tunnel" << endl;
        co_spawn(_accepting_socket.get_executor(),
                 [self = shared_from_this()] { return self->process_request(); },
                 detached);
    }

private:
    awaitable<void> process_request() {
        string request_string;
        std::size_t n = co_await asio::async_read_until(_accepting_socket,
                                                        asio::dynamic_buffer(request_string, 1024), "\r\n", use_awaitable);
        auto request = HttpConnectRequest::FromRequestString(request_string);
        // send reply
        string response = "HTTP/1.1 200 OK\r\n\r\n";
        co_await _accepting_socket.async_send(asio::buffer(response), use_awaitable);

        cout << "Connecting to host " << request.GetHost() << ", port " << request.GetPort() << endl;
        auto endpoints = co_await _resolver.async_resolve(request.GetHost(), request.GetPort(), use_awaitable);
        assert(endpoints.size() >= 1);

        co_await _target_socket.async_connect(*endpoints.begin(), use_awaitable);
        cout << "connected!" << endl;
        co_spawn(_io_context,
                 [self = shared_from_this()] { return self->relay_read(); },
                 detached);
        co_spawn(_io_context,
                 [self = shared_from_this()] { return self->relay_write(); },
                 detached);

    }

    awaitable<void> relay_read() {
        char data[16 * 1024];
        while (true) {
            //cout << "reading from source..." << endl;
            std::size_t n = co_await _accepting_socket.async_read_some(asio::buffer(data, sizeof(data)),
                                                           use_awaitable);
            //cout << "read " << n << " bytes from source. writing to target..." << endl;

            n = co_await _target_socket.async_write_some(asio::buffer(data, n), use_awaitable);
            //cout << "wrote " << n << " bytes to target" << endl;
        }
    }

    awaitable<void> relay_write() {
        char data[16 * 1024];
        while (true) {
            //cout << "reading from target..." << endl;
            std::size_t n = co_await _target_socket.async_read_some(asio::buffer(data, sizeof(data)),
                                                                       use_awaitable);

            //cout << "read " << n << " bytes from target. writing to source..." << endl;

            co_await _accepting_socket.async_write_some(asio::buffer(data, n), use_awaitable);
            //cout << "wrote " << n << " bytes to source" << endl;
        }
    }

private:
    tcp::socket _accepting_socket;
    tcp::socket _target_socket;
    tcp::resolver _resolver;
    asio::io_context& _io_context;
};

/**
 * TCP listener
 */
awaitable<void> listener(tcp::acceptor acceptor, asio::io_context& io_context)
{
    while (true) {
        cout << "waiting connection..." << endl;
        make_shared<HttpTunnel>(co_await acceptor.async_accept(use_awaitable), io_context)->start();
    }
}


// TODO: standardize unit testing framework
void test() {
    HttpConnectRequest t = HttpConnectRequest::FromRequestString("CONNECT some.domain.com:443 HTTP/1.1\r\n"
                          "Host: some.domain.com\r\n"
                          "User-Agent: test (unknown version) CFNetwork/1128.0.1 Darwin/19.6.0 (x86_64)\r\n"
                          "Connection: keep-alive\r\n"
                          "Proxy-Connection: keep-alive\r\n\r\n");
    assert(t.GetHost() == "some.domain.com");
    assert(t.GetPort() == "443");
}


int main() {

    test();

    try {
        asio::io_context io_context;

        const int port = 8080;
        co_spawn(io_context, listener(tcp::acceptor(io_context, {tcp::v4(), port}), io_context), detached);

        asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        //asio::executor_work_guard executor_word_guard(io_context);
        io_context.run();
    } catch (exception& e) {
        cout << "unhandled exception: " << e.what() << endl;
        return -1;
    }

    return 0;
}