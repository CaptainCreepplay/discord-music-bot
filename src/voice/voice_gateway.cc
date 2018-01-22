#include <boost/process.hpp>
#include <iostream>
#include <json.hpp>

#include <discord.h>
#include <net/resource_parser.h>
#include <voice/crypto.h>
#include <voice/opus_encoder.h>
#include <voice/voice_gateway.h>
#include <voice/voice_state_listener.h>

static void write_rtp_header(unsigned char *buffer, uint16_t seq_num, uint32_t timestamp,
                             uint32_t ssrc);

discord::voice_gateway::voice_gateway(boost::asio::io_context &ctx, discord::voice_gateway_entry &e,
                                      uint64_t user_id)
    : ctx{ctx}
    , websock{ctx}
    , sender{ctx, websock, 500}
    , entry{e}
    , socket{ctx}
    , resolver{ctx}
    , timer{ctx}
    , user_id{user_id}
    , ssrc{0}
    , udp_port{0}
    , buffer(1024)
    , timestamp{(uint32_t) rand()}
    , seq_num{(uint16_t) rand()}
    , state{connection_state::disconnected}
{
    std::cout << "[voice] connecting to gateway " << entry.endpoint << " user_id[" << user_id
              << " ] session_id[" << entry.session_id << "] token[" << entry.token << "]\n";

    socket.open(boost::asio::ip::udp::v4());
    std::cout << "[voice] created udp socket\n";
}

discord::voice_gateway::~voice_gateway()
{
    std::cout << "[voice] gateway destructor\n";
}

void discord::voice_gateway::connect(boost::asio::ip::tcp::resolver &resolver, error_cb c)
{
    voice_connect_callback = c;
    auto websock_connect_cb = [&](auto &ec) { on_connect(ec); };
    // Make sure we're using voice gateway v3
    websock.async_connect("wss://" + entry.endpoint + "/?v=3", resolver, websock_connect_cb);
}

void discord::voice_gateway::on_connect(const boost::system::error_code &ec)
{
    if (ec) {
        std::cerr << "[voice] websocket connect error: " << ec.message() << "\n";
        ctx.post([&]() { voice_connect_callback(ec); });
    } else {
        std::cout << "[voice] websocket connected\n";
        beater = std::make_unique<discord::heartbeater>(ctx, *this);
        identify();
    }
}

void discord::voice_gateway::identify()
{
    nlohmann::json identify{{"op", static_cast<int>(voice_op::identify)},
                            {"d",
                             {{"server_id", entry.guild_id},
                              {"user_id", user_id},
                              {"session_id", entry.session_id},
                              {"token", entry.token}}}};
    auto identify_sent_cb = [&](auto &ec, auto) {
        if (ec) {
            std::cout << "[voice] gateway identify error: " << ec.message() << "\n";
            ctx.post([&]() { voice_connect_callback(ec); });
        } else {
            std::cout << "[voice] starting event loop\n";
            event_loop();
        }
    };
    send(identify.dump(), identify_sent_cb);
}

void discord::voice_gateway::send(const std::string &s, transfer_cb c)
{
    sender.safe_send(s, c);
}

void discord::voice_gateway::event_loop()
{
    auto websock_msg_cb = [&](auto &ec, const auto *data, size_t len) {
        if (ec) {
            if (ec == websocket::error::websocket_connection_closed) {
                auto code = websock.close_code();
                ctx.post(
                    [&]() { voice_connect_callback(static_cast<voice_gateway::error>(code)); });
            } else {
                std::cerr << "[voice] gateway websocket read error: " << ec.message() << "\n";
                ctx.post([&]() { voice_connect_callback(ec); });
            }
        } else {
            handle_event(data, len);
        }
    };
    websock.async_next_message(websock_msg_cb);
}

void discord::voice_gateway::handle_event(const uint8_t *data, size_t len)
{
    const char *begin = reinterpret_cast<const char *>(data);
    const char *end = begin + len;

    std::cout << "[voice] ";
    std::cout.write(begin, len);
    std::cout << "\n";
    // Parse the results as a json object
    try {
        nlohmann::json json = nlohmann::json::parse(begin, end);
        auto payload = json.get<discord::voice_payload>();

        switch (payload.op) {
            case voice_op::ready:
                extract_ready_info(payload.data);
                break;
            case voice_op::session_description:
                extract_session_info(payload.data);
                break;
            case voice_op::speaking:
                break;
            case voice_op::heartbeat_ack:
                // We should check if the nonce is the same as the one sent by the
                // heartbeater
                beater->on_heartbeat_ack();
                break;
            case voice_op::hello:
                notify_heartbeater_hello(payload.data);
                break;
            case voice_op::resumed:
                // Successfully resumed
                state = connection_state::connected;
                break;
            case voice_op::client_disconnect:
                break;
            default:
                break;
        }
    } catch (std::exception &e) {
        std::cerr << "[voice] gateway error: " << e.what() << "\n";
    }
    event_loop();
}

void discord::voice_gateway::heartbeat()
{
    // TODO: save the nonce (rand()) and check if it is ACKed
    nlohmann::json json{{"op", static_cast<int>(voice_op::heartbeat)}, {"d", rand()}};
    send(json.dump(), ignore_transfer);
}

void discord::voice_gateway::resume()
{
    state = connection_state::disconnected;
    nlohmann::json resumed{{"op", static_cast<int>(voice_op::resume)},
                           {"d",
                            {{"server_id", entry.guild_id},
                             {"session_id", entry.session_id},
                             {"token", entry.token}}}};
    send(resumed.dump(), ignore_transfer);
}

void discord::voice_gateway::extract_ready_info(nlohmann::json &data)
{
    auto ready_info = data.get<discord::voice_ready>();
    ssrc = ready_info.ssrc;
    udp_port = ready_info.port;
    state = connection_state::connected;

    // Prepare buffer for ip discovery
    std::memset(buffer.data(), 0, 70);
    buffer[0] = (this->ssrc & 0xFF000000) >> 24;
    buffer[1] = (this->ssrc & 0x00FF0000) >> 16;
    buffer[2] = (this->ssrc & 0x0000FF00) >> 8;
    buffer[3] = (this->ssrc & 0x000000FF);

    std::string host;
    // Parse the endpoint url, extracting only the host
    auto parsed_results = resource_parser::parse(entry.endpoint);
    host = parsed_results.host;
    boost::asio::ip::udp::resolver::query query{boost::asio::ip::udp::v4(), host,
                                                std::to_string(udp_port)};
    resolver.async_resolve(query, [&](const boost::system::error_code &e,
                                      boost::asio::ip::udp::resolver::iterator iterator) {
        if (e) {
            ctx.post([&]() { voice_connect_callback(e); });
        } else {
            send_endpoint = *iterator;
            ip_discovery();
        }
    });
}

void discord::voice_gateway::extract_session_info(nlohmann::json &data)
{
    auto session_info = data.get<discord::voice_session>();
    if (session_info.mode != "xsalsa20_poly1305")
        throw std::runtime_error("Unsupported voice mode: " + session_info.mode);

    secret_key = std::move(session_info.secret_key);

    if (secret_key.size() != 32)
        throw std::runtime_error("Expected 32 byte secret key but got " +
                                 std::to_string(secret_key.size()));

    // We are ready to start speaking!
    ctx.post([&]() { voice_connect_callback({}); });
}

void discord::voice_gateway::ip_discovery()
{
    // Send buffer over socket, timing out after in case of packet loss
    // Receive 70 byte payload containing external ip and udp portno

    // Let's try retry 5 times if we fail to receive response
    retries = 5;

    send_ip_discovery_datagram();
    auto udp_recv_cb = [&](auto &ec, auto transferred) {
        if (ec) {
            ctx.post([&]() { voice_connect_callback(ec); });
        } else if (transferred >= 70) {
            // We got our response, cancel the next send
            timer.cancel();

            // First 4 bytes of buffer should be SSRC, next is beginning
            // of this udp socket's external IP
            external_ip = std::string((char *) &buffer[4]);

            // Extract the port the udp socket is on (little-endian)
            uint16_t local_udp_port = (buffer[69] << 8) | buffer[68];

            std::cout << "[voice] udp socket bound at " << external_ip << ":" << local_udp_port
                      << "\n";

            select(local_udp_port);
        }
    };

    // udp_recv_cb isn't called until the socket is closed, or the data is received
    socket.async_receive_from(boost::asio::buffer(buffer, buffer.size()), receive_endpoint,
                              udp_recv_cb);
}

void discord::voice_gateway::send_ip_discovery_datagram()
{
    auto udp_sent_cb = [&](auto &ec, auto) {
        if (ec && ec != boost::asio::error::operation_aborted) {
            std::cerr << "[voice] could not send udp packet to voice server: " << ec.message()
                      << "\n";
        }
        if (retries == 0) {
            // Alert the caller that we failed
            ctx.post([&]() { voice_connect_callback(voice_gateway::error::ip_discovery_failed); });
            // close the socket to complete the async_receive_from
            socket.close();
            return;
        }
        retries--;
        auto timer_cb = [&](auto &e) {
            if (!e)
                send_ip_discovery_datagram();
        };
        // Next time expires in 200 ms
        timer.expires_from_now(boost::posix_time::milliseconds(200));
        timer.async_wait(timer_cb);
    };
    socket.async_send_to(boost::asio::buffer(buffer.data(), 70), send_endpoint, udp_sent_cb);
}

void discord::voice_gateway::select(uint16_t local_udp_port)
{
    nlohmann::json select_payload{
        {"op", static_cast<int>(voice_op::select_proto)},
        {"d",
         {{"protocol", "udp"},
          {"data",
           {{"address", external_ip}, {"port", local_udp_port}, {"mode", "xsalsa20_poly1305"}}}}}};

    send(select_payload.dump(), ignore_transfer);
}

void discord::voice_gateway::notify_heartbeater_hello(nlohmann::json &data)
{
    // Override the heartbeat_interval with value 75% of current
    // This is a bug with Discord apparently
    if (data["heartbeat_interval"].is_number()) {
        int val = data.at("heartbeat_interval").get<int>();
        val = (val / 4) * 3;
        data["heartbeat_interval"] = val;
        beater->on_hello(data);
    }
}

static void do_speak(discord::voice_gateway &vg, transfer_cb c, bool speak)
{
    // Apparently this _doesnt_ need the ssrc
    nlohmann::json speaking_payload{{"op", static_cast<int>(discord::voice_op::speaking)},
                                    {"d", {{"speaking", speak}, {"delay", 0}}}};
    vg.send(speaking_payload.dump(), c);
}

void discord::voice_gateway::start_speaking(transfer_cb c)
{
    do_speak(*this, c, true);
}

void discord::voice_gateway::stop_speaking(transfer_cb c)
{
    do_speak(*this, c, false);
}

void discord::voice_gateway::play(audio_frame frame)
{
    if (!is_speaking) {
        auto speak_sent_cb = [=](auto &ec, auto) {
            if (!ec) {
                std::cout << "[voice] now speaking\n";
                is_speaking = true;
                send_audio(frame);
            }
        };
        start_speaking(speak_sent_cb);
    } else {
        send_audio(frame);
    }
}

void discord::voice_gateway::stop()
{
    is_speaking = false;
    std::cout << "[voice] stopped speaking\n";
    stop_speaking(ignore_transfer);
}

void discord::voice_gateway::send_audio(audio_frame frame)
{
    // Make sure we have enough room to store the encoded audio, 12 bytes for RTP header,
    // crypto_secretbo_MACBYTES (for MAC) in buffer
    if (frame.encoded_len + 12 + crypto_secretbox_MACBYTES > buffer.size())
        buffer.resize(frame.encoded_len + 12 + crypto_secretbox_MACBYTES);

    uint8_t *buf = buffer.data();
    auto write_audio = &buf[12];
    uint8_t nonce[24];

    write_rtp_header(buf, seq_num, timestamp, ssrc);

    // First 12 bytes of nonce are RTP header, next 12 are 0s
    std::memcpy(nonce, buf, 12);
    std::memset(&nonce[12], 0, 12);

    seq_num++;
    timestamp += frame.frame_count;

    auto error = discord::crypto::xsalsa20_poly1305_encrypt(
        frame.opus_encoded_data, write_audio, frame.encoded_len, secret_key.data(), nonce);

    if (error) {
        std::cerr << "[voice] error encrypting data\n";
        return;  // There was a problem encrypting the data
    }

    // Make sure we also count the RTP header and the MAC from encrypting
    frame.encoded_len += 12 + crypto_secretbox_MACBYTES;

    socket.async_send_to(boost::asio::buffer(buf, frame.encoded_len), send_endpoint, ignore_transfer);
    std::cout << "[RTP] " << frame.encoded_len << " bytes sent\r";
}

const char *discord::voice_gateway::error_category::name() const noexcept
{
    return "voice_gateway";
}

std::string discord::voice_gateway::error_category::message(int ev) const noexcept
{
    switch (voice_gateway::error(ev)) {
        case voice_gateway::error::ip_discovery_failed:
            return "ip discovery failed";
        case voice_gateway::error::unknown_opcode:
            return "invalid opcode";
        case voice_gateway::error::not_authenticated:
            return "sent payload before identified";
        case voice_gateway::error::authentication_failed:
            return "incorrect token in identify payload";
        case voice_gateway::error::already_authenticated:
            return "sent more than one identify payload";
        case voice_gateway::error::session_no_longer_valid:
            return "session is no longer valid";
        case voice_gateway::error::session_timeout:
            return "session has timed out";
        case voice_gateway::error::server_not_found:
            return "server not found";
        case voice_gateway::error::unknown_protocol:
            return "unrecognized protocol";
        case voice_gateway::error::disconnected:
            return "disconnected";
        case voice_gateway::error::voice_server_crashed:
            return "voice server crashed";
        case voice_gateway::error::unknown_encryption_mode:
            return "unrecognized encryption";
    }
    return "Unknown voice gateway error";
}

bool discord::voice_gateway::error_category::equivalent(const boost::system::error_code &code,
                                                        int condition) const noexcept
{
    return &code.category() == this && static_cast<int>(code.value()) == condition;
}

const boost::system::error_category &discord::voice_gateway::error_category::instance()
{
    static voice_gateway::error_category instance;
    return instance;
}

boost::system::error_code discord::make_error_code(discord::voice_gateway::error code) noexcept
{
    return {(int) code, discord::voice_gateway::error_category::instance()};
}

static void write_rtp_header(unsigned char *buffer, uint16_t seq_num, uint32_t timestamp,
                             uint32_t ssrc)
{
    buffer[0] = 0x80;
    buffer[1] = 0x78;

    buffer[2] = (seq_num & 0xFF00) >> 8;
    buffer[3] = (seq_num & 0x00FF);

    buffer[4] = (timestamp & 0xFF000000) >> 24;
    buffer[5] = (timestamp & 0x00FF0000) >> 16;
    buffer[6] = (timestamp & 0x0000FF00) >> 8;
    buffer[7] = (timestamp & 0x000000FF);

    buffer[8] = (ssrc & 0xFF000000) >> 24;
    buffer[9] = (ssrc & 0x00FF0000) >> 16;
    buffer[10] = (ssrc & 0x0000FF00) >> 8;
    buffer[11] = (ssrc & 0x000000FF);
}
