#pragma once
#include <qunet/socket/Socket.hpp>
#include <charconv>

namespace qn {

enum class UrlParseError {
    Success,
    InvalidProtocol,
    InvalidPort,
};

class UrlParser {
public:
    UrlParser(std::string_view url) : m_url(url) {
        this->parse();
    }

    void parse() {
        auto protoEnd = m_url.find("://");
        ConnectionType type = ConnectionType::Unknown; // Unknown is qunet

        if (protoEnd != m_url.npos) {
            std::string_view proto = m_url.substr(0, protoEnd);
            if (proto == "udp") {
                type = ConnectionType::Udp;
            } else if (proto == "tcp") {
                type = ConnectionType::Tcp;
            } else if (proto == "quic") {
                type = ConnectionType::Quic;
            } else if (proto == "qunet") {
                type = ConnectionType::Unknown;
            } else {
                m_result = UrlParseError::InvalidProtocol;
                return; // Invalid protocol
            }

            m_url = m_url.substr(protoEnd + 3); // skip the protocol path
        }

        m_proto = type;

        // if there's a trailing slash, remove it
        while (!m_url.empty() && m_url.back() == '/') {
            m_url.remove_suffix(1);
        }

        bool hasPort = false;
        auto colonPos = m_url.find_last_of(':');
        if (colonPos != std::string::npos) {
            // check if the colon is preceded by ']' (e.g. qunet://[::1]:1234)

            if (m_url[colonPos - 1] == ']') {
                // ipv6 address with a port
                hasPort = true;
            } else {
                // if it's an ipv6 address, then this is not a port and instead a part of the address
                // if it's an ipv4 address or a domain, then this is a port
                if (qsox::Ipv6Address::parse(std::string(m_url))) {
                    hasPort = false;
                } else {
                    hasPort = true;
                }
            }
        }

        std::optional<uint16_t> port;
        if (hasPort) {
            auto portStr = m_url.substr(colonPos + 1);
            uint16_t p;

            if (std::from_chars(&*portStr.begin(), &*portStr.end(), p).ec != std::errc()) {
                m_result = UrlParseError::InvalidPort;
                return;
            }

            m_url.remove_suffix(portStr.size() + 1); // remove the port part
            port = p;
        }

        if (m_url.starts_with('[') && m_url.ends_with(']')) {
            m_url.remove_prefix(1);
            m_url.remove_suffix(1);
        }

        if (auto address = qsox::IpAddress::parse(std::string(m_url))) {
            if (port) {
                m_value = qsox::SocketAddress{*address, *port};
            } else {
                m_value = *address;
            }
        } else if (port) {
            m_value = std::make_pair(m_url, *port);
        } else {
            m_value = m_url;
        }
    }

    UrlParseError result() const {
        return m_result;
    }

    std::optional<ConnectionType> protocol() const {
        return m_proto;
    }

    bool isIpWithPort() const {
        return m_value && std::holds_alternative<qsox::SocketAddress>(*m_value);
    }

    bool isIp() const {
        return m_value && std::holds_alternative<qsox::IpAddress>(*m_value);
    }

    bool isDomain() const {
        return m_value && std::holds_alternative<std::string_view>(*m_value);
    }

    bool isDomainWithPort() const {
        return m_value && std::holds_alternative<std::pair<std::string_view, uint16_t>>(*m_value);
    }

    qsox::SocketAddress& asIpWithPort() {
        return std::get<qsox::SocketAddress>(*m_value);
    }

    qsox::IpAddress& asIp() {
        return std::get<qsox::IpAddress>(*m_value);
    }

    std::string_view& asDomain() {
        return std::get<std::string_view>(*m_value);
    }

    std::pair<std::string_view, uint16_t>& asDomainWithPort() {
        return std::get<std::pair<std::string_view, uint16_t>>(*m_value);
    }

private:
    std::string_view m_url;
    std::optional<ConnectionType> m_proto;
    UrlParseError m_result;

    std::optional<std::variant<
        qsox::SocketAddress, // ip with port
        qsox::IpAddress, // ip
        std::string_view, // domain
        std::pair<std::string_view, uint16_t> // domain with port
    >> m_value;
};

}