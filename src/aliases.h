#ifndef DISCORD_ALIASES_H
#define DISCORD_ALIASES_H

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using udp = boost::asio::ip::udp;
using ssl_stream = ssl::stream<tcp::socket>;
using secure_websocket = boost::beast::websocket::stream<ssl_stream>;

#endif
