#include "net_proc.h"
#include "kernel_processes/kprocess_loader.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "std/memfunctions.h"
#include "std/string.h"
#include "net/internet_layer/ipv4.h"
#include "net/transport_layer/csocket_udp.h"
#include "net/application_layer/csocket_http_client.h"
#include "net/application_layer/csocket_http_server.h"
#include "net/application_layer/dhcp_daemon.h"
#include "net/network_types.h"
#include "net/link_layer/arp.h"
#include "networking/network.h"
#include "net/net.h"

extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);
extern void      sleep(uint64_t ms);

#define KP(fmt, ...) \
    do { kprintf(fmt, ##__VA_ARGS__); } while (0)

static uint32_t pick_probe_ip() {
    const net_cfg_t *cfg = ipv4_get_cfg();
    if (!cfg || cfg->mode == NET_MODE_DISABLED || cfg->ip == 0)
        return 0;
    if (cfg->gw)
        return cfg->gw;
    uint32_t bcast = ipv4_broadcast(cfg->ip, cfg->mask);
    if (bcast)
        return bcast;
    return ipv4_first_host(cfg->ip, cfg->mask);
}

static int udp_probe_server(uint32_t probe_ip,
                            uint16_t probe_port,
                            net_l2l3_endpoint *out_l2,
                            net_l4_endpoint   *out_l4)
{
    const net_l2l3_endpoint *local = network_get_local_endpoint();
    if (!local)
        return 0;

    socket_handle_t sock = udp_socket_create(0, 0);
    if (!sock)
        return 0;

    const char greeting[] = "hello";
    if (socket_sendto_udp(sock, probe_ip, probe_port, greeting, sizeof(greeting)) < 0) {
        socket_destroy_udp(sock);
        return 0;
    }

    char recv_buf[64];
    uint32_t waited = 0;
    const uint32_t TIMEOUT_MS = 1000;
    const uint32_t INTERVAL_MS = 50;
    int64_t  recvd = 0;
    uint32_t resp_ip = 0;
    uint16_t resp_port = 0;

    while (waited < TIMEOUT_MS) {
        recvd = socket_recvfrom_udp(sock,
                                    recv_buf,
                                    sizeof(recv_buf),
                                    &resp_ip,
                                    &resp_port);
        if (recvd > 0)
            break;
        sleep(INTERVAL_MS);
        waited += INTERVAL_MS;
    }

    if (recvd <= 0) {
        socket_close_udp(sock);
        socket_destroy_udp(sock);
        return 0;
    }

    socket_close_udp(sock);
    socket_destroy_udp(sock);

    memcpy(out_l2->mac, local->mac, 6);
    out_l2->ip = resp_ip;
    out_l4->ip = resp_ip;
    out_l4->port = resp_port;

    return resp_ip;
}


void free_request(HTTPRequestMsg *req)
{
    if (req->path.mem_length)
        free(req->path.data, req->path.mem_length);

    for (uint32_t i = 0; i < req->extra_header_count; ++i) {
        HTTPHeader *h = &req->extra_headers[i];
        if (h->key.mem_length)
            free(h->key.data, h->key.mem_length);
        if (h->value.mem_length)
            free(h->value.data, h->value.mem_length);
    }

    if (req->extra_headers)
        free(req->extra_headers, req->extra_header_count * sizeof(HTTPHeader));

    if (req->body.ptr && req->body.size)
        free((void*)req->body.ptr, req->body.size);
}

void http_server_hello_entry()
{
    uint16_t pid = get_current_proc_pid();
    http_server_handle_t srv = http_server_create(pid);
    if (!srv) {
        stop_current_process();
        return;
    }

    if (http_server_bind(srv, 80) < 0) {
        http_server_destroy(srv);
        stop_current_process();
        return;
    }

    if (http_server_listen(srv, 4) < 0) {
        http_server_close(srv);
        http_server_destroy(srv);
        stop_current_process();
        return;
    }

    KP("[HTTP] listening at %i.%i.%i.%i on port 80", FORMAT_IP(ipv4_get_cfg()->ip));

    static const char HTML_ROOT[] =
        "<h1>Hello, world!</h1>\n"
        "<h3>[Redacted]</h3>";

    static const char HTML_404[] =
        "<h1>404 Regrettably, no such page exists in this realm</h1>\n"
        "<p>Im rather inclined to deduce that your page simply does not exist. Given the state of affairs, I dare say it's not altogether surprising, innit?</p>";
        //comically british 404 error page
    const string STR_OK = string_from_const("OK");
    const string STR_HTML = string_from_const("text/html");
    const string STR_CLOSE = string_from_const("close");
    const string STR_NOTFOUND = string_from_const("Not Found");

    while (1) {
        http_connection_handle_t conn = http_server_accept(srv);
        if (!conn)
            continue;

        HTTPRequestMsg req = http_server_recv_request(srv, conn);

        if (req.path.length) {
            char tmp[128] = {0};
            uint32_t n = req.path.length < sizeof(tmp) - 1
                        ? req.path.length
                        : sizeof(tmp) - 1;
            memcpy(tmp, req.path.data, n);
            KP("[HTTP] GET %s", tmp);
        }

        HTTPResponseMsg res = {0};

        if (req.path.length == 1 && req.path.data[0] == '/') {
            res.status_code = HTTP_OK;
            res.reason = STR_OK;
            res.headers_common.length = sizeof(HTML_ROOT) - 1;
            res.headers_common.type = STR_HTML;
            res.headers_common.connection = STR_CLOSE;
            res.body.ptr = (uintptr_t)HTML_ROOT;
            res.body.size = sizeof(HTML_ROOT) - 1;
        }
        else {
            res.status_code = HTTP_NOT_FOUND;
            res.reason = STR_NOTFOUND;
            res.headers_common.length = sizeof(HTML_404) - 1;
            res.headers_common.type = STR_HTML;
            res.headers_common.connection = STR_CLOSE;
            res.body.ptr = (uintptr_t)HTML_404;
            res.body.size = sizeof(HTML_404) - 1;
        }

        http_server_send_response(srv, conn, &res);
        http_connection_close(conn);
        free_request(&req);
    }
}



static void test_http(uint32_t ip)
{
    KP("[HTTP] GET %i.%i.%i.%i:80\n",
       (ip >> 24) & 0xFF,
       (ip >> 16) & 0xFF,
       (ip >>  8) & 0xFF,
       (ip ) & 0xFF);

    uint16_t pid = get_current_proc_pid();
    http_client_handle_t cli = http_client_create(pid);
    if (!cli)
        return;

    if (http_client_connect(cli, ip, 80) < 0) {
        http_client_destroy(cli);
        return;
    }

    HTTPRequestMsg req = {0};
    req.method = HTTP_METHOD_GET;
    req.path = string_from_const("/");
    req.headers_common.connection = string_from_const("close");

    HTTPResponseMsg resp = http_client_send_request(cli, &req);
    free(req.path.data, req.path.mem_length);
    free(req.headers_common.connection.data, req.headers_common.connection.mem_length);

    if (resp.body.ptr && resp.body.size > 0) {
        char *body_str = (char*)malloc(resp.body.size + 1);
        if (body_str) {
            memcpy(body_str, (void*)resp.body.ptr, resp.body.size);
            body_str[resp.body.size] = '\0';
            KP("[HTTP] %i %i bytes of body%s\n",
               (uint64_t)resp.status_code,
               (uint64_t)resp.body.size,
               body_str);
            free(body_str, resp.body.size + 1);
        }
    }

    http_client_close(cli);
    http_client_destroy(cli);
    if (resp.reason.data && resp.reason.mem_length)
        free(resp.reason.data, resp.reason.mem_length);
    for (uint32_t i = 0; i < resp.extra_header_count; i++) {
        HTTPHeader *h = &resp.extra_headers[i];
        if (h->key.mem_length)
            free(h->key.data, h->key.mem_length);
        if (h->value.mem_length)
            free(h->value.data, h->value.mem_length);
    }
    if (resp.extra_headers)
        free(resp.extra_headers,
             resp.extra_header_count * sizeof(HTTPHeader));
}

void test_network()
{
    const net_cfg_t *cfg = ipv4_get_cfg();
    net_l2l3_endpoint l2 = {0};
    net_l4_endpoint srv = {0};

    if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
        uint32_t bcast = ipv4_broadcast(cfg->ip, cfg->mask);
        KP("[NET] probing broadcast %i.%i.%i.%i",
           (bcast>>24)&0xFF,(bcast>>16)&0xFF,
           (bcast>>8)&0xFF,(bcast&0xFF));

        if (udp_probe_server(bcast, 8080, &l2, &srv)) {
            test_http(srv.ip);
        }
        http_server_hello_entry();
        return;
    }

    uint32_t fallback = pick_probe_ip();
    if (!fallback)
        fallback = (192<<24)|(168<<16)|(1<<8)|255;
    if (udp_probe_server(fallback, 8080, &l2, &srv)) {
        test_http(srv.ip);
    } else {
        KP("[NET] could not find update server\n");
    }
}

void net_test_entry(){
    test_network();
    stop_current_process();
}

void ip_waiter_entry()
{
    for (;;) {
        const net_cfg_t *cfg = ipv4_get_cfg();
        if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
            create_kernel_process("net_test", net_test_entry);
            break;
        }
        sleep(200);
    }
    stop_current_process();
}

process_t* launch_net_process()
{
    const net_cfg_t *cfg = ipv4_get_cfg();

    process_t* net  = create_kernel_process("net_net",  network_net_task_entry);
    network_net_set_pid(net ? net->id : 0xFFFF);

    process_t* arp = create_kernel_process("arp_daemon", arp_daemon_entry);
    arp_set_pid(arp ? arp->id : 0xFFFF);

    if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
        create_kernel_process("net_test", net_test_entry);
        return NULL;
    }

    process_t* dhcp = create_kernel_process("dhcp_daemon", dhcp_daemon_entry);
    dhcp_set_pid(dhcp ? dhcp->id : 0xFFFF);
    create_kernel_process("ip_waiter", ip_waiter_entry);
    return dhcp;
}
