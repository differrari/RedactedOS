#include "csocket_http_server.h"
#include "socket_http_server.hpp"
#include "networking/transport_layer/socket_tcp.hpp"
#include "networking/transport_layer/socket.hpp"


extern "C" {

http_server_handle_t http_server_create(uint16_t pid, const SocketExtraOptions* extra) {
    HTTPServer* srv = new HTTPServer(pid, extra);
    if (!srv) return nullptr;
    return reinterpret_cast<http_server_handle_t>(srv);
}

void http_server_destroy(http_server_handle_t h) {
    if (!h) return;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    delete srv;
}

int32_t http_server_bind(http_server_handle_t h, const SockBindSpec *spec, uint16_t port) {
    if (!h || !spec) return (int32_t)SOCK_ERR_INVAL;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    return srv->bind(*spec, port);
}

int32_t http_server_listen(http_server_handle_t h, int backlog) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    return srv->listen(backlog);
}

http_connection_handle_t http_server_accept(http_server_handle_t h) {
    if (!h) return nullptr;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    TCPSocket* cli = srv->accept();
    return reinterpret_cast<http_connection_handle_t>(cli);
}

HTTPRequestMsg http_server_recv_request(http_server_handle_t h, http_connection_handle_t c) {
    HTTPRequestMsg empty{};
    if (!h || !c) return empty;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    TCPSocket* conn = reinterpret_cast<TCPSocket*>(c);
    return srv->recv_request(conn);
}

int32_t http_server_send_response(http_server_handle_t h, http_connection_handle_t c, const HTTPResponseMsg *res) {
    if (!h || !c || !res) return (int32_t)SOCK_ERR_INVAL;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    TCPSocket* conn = reinterpret_cast<TCPSocket*>(c);
    return srv->send_response(conn, *res);
}

int32_t http_connection_close(http_connection_handle_t c) {
    if (!c) return (int32_t)SOCK_ERR_INVAL;
    TCPSocket* conn = reinterpret_cast<TCPSocket*>(c);
    delete conn;
    return (int32_t)SOCK_OK;
}

int32_t http_server_close(http_server_handle_t h) {
    if (!h) return (int32_t)SOCK_ERR_INVAL;
    HTTPServer* srv = reinterpret_cast<HTTPServer*>(h);
    return srv->close();
}

}
