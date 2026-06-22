#include "sylar/socket.h"

int main()
{
    auto socket = sylar::SSLSocket::CreateTCPSocket();
    if (!socket || !socket->getClientOptions().verify_peer)
    {
        return 1;
    }

    sylar::SSLSocket::ClientOptions options;
    options.server_name = "api.example.test";
    options.ca_file = "/tmp/test-ca.pem";
    options.verify_peer = false;
    socket->setClientOptions(options);

    const auto &actual = socket->getClientOptions();
    return actual.server_name == options.server_name && actual.ca_file == options.ca_file &&
                   actual.verify_peer == options.verify_peer
               ? 0
               : 1;
}
