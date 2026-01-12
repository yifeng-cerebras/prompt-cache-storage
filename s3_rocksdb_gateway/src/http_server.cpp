#include "http_server.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace server {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket socket, s3::Api& api, std::size_t max_body, Metrics* metrics)
    : socket_(std::move(socket)), api_(api), max_body_(max_body), metrics_(metrics) {}

  void run() {
    beast::error_code ec;
    socket_.set_option(tcp::no_delay(true), ec);
    do_read();
  }

private:
  void do_read() {
    parser_.emplace();
    parser_->body_limit(max_body_);
    request_start_ = std::chrono::steady_clock::now();
    auto self = shared_from_this();
    http::async_read(socket_, buffer_, *parser_,
      [self](beast::error_code ec, std::size_t bytes) {
        self->on_read(ec, bytes);
      });
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) return do_close();
    if (ec) return; // drop

    const auto& req = parser_->get();
    if (metrics_) metrics_->IncInFlight();

    if (req.method() == http::verb::get &&
        std::string_view(req.target().data(), req.target().size()) == "/metrics") {
      const std::string body = metrics_ ? metrics_->RenderPrometheus() : std::string();
      res_ = s3::Response{http::status::ok, req.version()};
      res_.set(http::field::content_type, "text/plain; version=0.0.4");
      res_.keep_alive(req.keep_alive());
      res_.body().assign(body.begin(), body.end());
      res_.content_length(res_.body().size());
    } else {
      res_ = api_.handle(req);
    }

    if (metrics_) {
      stats_.method.assign(req.method_string().data(), req.method_string().size());
      stats_.status = res_.result_int();
      stats_.req_bytes = req.body().size();
      stats_.resp_bytes = res_.body().size();
    }

    auto self = shared_from_this();
    const bool close = res_.need_eof();
    http::async_write(socket_, res_,
      [self, close](beast::error_code ec, std::size_t bytes) {
        self->on_write(close, ec, bytes);
      });
  }

  void on_write(bool close, beast::error_code ec, std::size_t) {
    if (metrics_) {
      const auto end = std::chrono::steady_clock::now();
      const double latency_ms =
        std::chrono::duration<double, std::milli>(end - request_start_).count();
      metrics_->Observe(stats_.method, stats_.status, stats_.req_bytes, stats_.resp_bytes, latency_ms);
      metrics_->DecInFlight();
    }
    if (ec) return;
    if (close) return do_close();
    do_read();
  }

  void do_close() {
    beast::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_send, ec);
  }

  tcp::socket socket_;
  beast::flat_buffer buffer_;
  s3::Api& api_;
  std::size_t max_body_;
  std::optional<http::request_parser<s3::Request::body_type>> parser_;
  s3::Response res_;
  Metrics* metrics_ = nullptr;
  std::chrono::steady_clock::time_point request_start_{};
  struct RequestStats {
    std::string method;
    unsigned status = 0;
    std::size_t req_bytes = 0;
    std::size_t resp_bytes = 0;
  } stats_;
};

Listener::Listener(asio::io_context& ioc, tcp::endpoint endpoint, s3::Api& api, Config cfg)
  : ioc_(ioc), acceptor_(ioc), api_(api), cfg_(std::move(cfg)) {
  beast::error_code ec;
  acceptor_.open(endpoint.protocol(), ec);
  if (ec) return;
  acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) return;
  acceptor_.bind(endpoint, ec);
  if (ec) return;
  acceptor_.listen(asio::socket_base::max_listen_connections, ec);
}

void Listener::run() {
  do_accept();
}

void Listener::do_accept() {
  auto self = shared_from_this();
  acceptor_.async_accept(
    [self](beast::error_code ec, tcp::socket socket) {
      self->on_accept(ec, std::move(socket));
    });
}

void Listener::on_accept(beast::error_code ec, tcp::socket socket) {
  if (!ec) {
    std::make_shared<Session>(std::move(socket), api_, cfg_.max_request_body_bytes, cfg_.metrics)->run();
  }
  do_accept();
}

} // namespace server
