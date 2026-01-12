#pragma once

#include "metrics.hpp"
#include "s3_api.hpp"

#include <boost/asio.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace server {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

struct Config {
  std::string listen_host = "0.0.0.0";
  unsigned short listen_port = 9000;
  std::size_t max_request_body_bytes = 64u * 1024u * 1024u;
  Metrics* metrics = nullptr;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context& ioc, tcp::endpoint endpoint, s3::Api& api, Config cfg);

  void run();

private:
  void do_accept();
  void on_accept(boost::system::error_code ec, tcp::socket socket);

  asio::io_context& ioc_;
  tcp::acceptor acceptor_;
  s3::Api& api_;
  Config cfg_;
};

} // namespace server
