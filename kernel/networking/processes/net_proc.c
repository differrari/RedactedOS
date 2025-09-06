#include "net_proc.h"
#include "kernel_processes/kprocess_loader.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "std/memory.h"
#include "std/string.h"

#include "net/network_types.h"
#include "networking/network.h"
#include "networking/interface_manager.h"

#include "net/link_layer/arp.h"

#include "net/internet_layer/ipv4.h"

#include "net/transport_layer/csocket_udp.h"

#include "net/application_layer/csocket_http_client.h"
#include "net/application_layer/csocket_http_server.h"
#include "net/application_layer/dhcp_daemon.h"
#include "net/application_layer/dns_daemon.h"
#include "net/application_layer/dns.h"
#include "net/application_layer/sntp_daemon.h"

#include "exceptions/timer.h"
extern uintptr_t malloc(uint64_t size);
extern void free(void *ptr, uint64_t size);
extern void sleep(uint64_t ms);

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

static int udp_probe_server(uint32_t probe_ip, uint16_t probe_port, net_l4_endpoint *out_l4) {
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
    const uint32_t TIMEOUT_MS = 2000;
    const uint32_t INTERVAL_MS = 50;
    int64_t recvd = 0;
    uint32_t resp_ip = 0;
    uint16_t resp_port = 0;

    while (waited < TIMEOUT_MS) {
        recvd = socket_recvfrom_udp(sock, recv_buf, sizeof(recv_buf), &resp_ip, &resp_port);
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

    out_l4->ip = resp_ip;
    out_l4->port = resp_port;

    return resp_ip;
}

static void free_request(HTTPRequestMsg *req) {
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

static void run_http_server() {
    uint16_t pid = get_current_proc_pid();
    http_server_handle_t srv = http_server_create(pid);
    if (!srv) {
        stop_current_process(1);
        return;
    }

    if (http_server_bind(srv, 80) < 0) {
        http_server_destroy(srv);
        stop_current_process(2);
        return;
    }

    if (http_server_listen(srv, 4) < 0) {
        http_server_close(srv);
        http_server_destroy(srv);
        stop_current_process(3);
        return;
    }

    kprintf("[HTTP] listening on port 80");

    static const char HTML_ROOT[] =
        "<h1>Hello, world!</h1>\n"
        "<h3>[Redacted]</h3>";

    static const char HTML_404[] =
        "<h1>404 Regrettably, no such page exists in this realm</h1>\n"
        "<p>Im rather inclined to deduce that your page simply does not exist. Given the state of affairs, I dare say it's not altogether surprising, innit?</p>";

    const string STR_OK      = string_from_const("OK");
    const string STR_HTML    = string_from_const("text/html");
    const string STR_CLOSE   = string_from_const("close");
    const string STR_NOTFOUND= string_from_const("Not Found");

    while (1) {
        http_connection_handle_t conn = http_server_accept(srv);
        if (!conn)
            continue;

        HTTPRequestMsg req = http_server_recv_request(srv, conn);

        if (req.path.length) {
            char tmp[128] = {0};
            uint32_t n = req.path.length < sizeof(tmp) - 1 ? req.path.length : sizeof(tmp) - 1;
            memcpy(tmp, req.path.data, n);
            kprintf("[HTTP] GET %s", tmp);
        }

        HTTPResponseMsg res = {0};

        if (req.path.length == 1 && req.path.data[0] == '/') {
            res.status_code = HTTP_OK;
            res.reason = STR_OK;
            res.headers_common.length = sizeof(HTML_ROOT) - 1;
            res.headers_common.type = STR_HTML;
            res.headers_common.connection = STR_CLOSE;
            res.body.ptr  = (uintptr_t)HTML_ROOT;
            res.body.size = sizeof(HTML_ROOT) - 1;
        } else {
            res.status_code = HTTP_NOT_FOUND;
            res.reason = STR_NOTFOUND;
            res.headers_common.length = sizeof(HTML_404) - 1;
            res.headers_common.type = STR_HTML;
            res.headers_common.connection = STR_CLOSE;
            res.body.ptr  = (uintptr_t)HTML_404;
            res.body.size = sizeof(HTML_404) - 1;
        }

        http_server_send_response(srv, conn, &res);
        http_connection_close(conn);
        free_request(&req);
    }
}

static void test_http(uint32_t ip) {
    char ip_str[16];
    ipv4_to_string(ip, ip_str);

    kprintf("[HTTP] GET %s:80\n", ip_str);
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
            kprintf("[HTTP] %i %i bytes of body", resp.status_code, resp.body.size);
            kprintf("%s", body_str);
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
        free(resp.extra_headers, resp.extra_header_count * sizeof(HTTPHeader));
}

static void print_info() {
    if (!sntp_is_running()) {
        create_kernel_process("sntpd", sntp_daemon_entry, 0, 0);
        while (!timer_is_synchronised());

        const net_cfg_t *cfg = ipv4_get_cfg();
        if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
            char ip_str[16], mask_str[16], gw_str[16];
            ipv4_to_string(cfg->ip,   ip_str);
            ipv4_to_string(cfg->mask, mask_str);
            ipv4_to_string(cfg->gw,   gw_str);

            kprintf("[NET] IP: %s MASK: %s GW: %s", ip_str, mask_str, gw_str);
        }
        kprintf("[NET] PIDs -- NET: %i ARP: %i DHCP: %i DNS: %i SNTP: %i",
            network_net_get_pid(),
            arp_get_pid(),
            dhcp_get_pid(),
            dns_get_pid(),
            sntp_get_pid());

        timer_set_timezone_minutes(120);
        kprintf("[TIME]timezone offset %i minutes", (int32_t)timer_get_timezone_minutes());

        DateTime now_dt_utc, now_dt_loc;
        if (timer_now_datetime(&now_dt_utc, 0)) {
            char s[20];
            timer_datetime_to_string(&now_dt_utc, s, sizeof s);
            kprintf("[TIME] UTC: %s", s);
        }
        if (timer_now_datetime(&now_dt_loc, 1)) {
            char s[20];
            timer_datetime_to_string(&now_dt_loc, s, sizeof s);
            kprintf("[TIME] LOCAL: %s (TZ %i min)", s, (int32_t)timer_get_timezone_minutes());
        }
    }
}

static void test_net() {
    const net_cfg_t *cfg = ipv4_get_cfg();
    net_l4_endpoint srv = {0};

    if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
        print_info();

        uint32_t bcast = ipv4_broadcast(cfg->ip, cfg->mask);
        char bcast_str[16];
        ipv4_to_string(bcast, bcast_str);

        kprintf("[NET] probing broadcast %s", bcast_str);

        if (udp_probe_server(bcast, 8080, &srv))
            test_http(srv.ip);

        sleep(2000);
        run_http_server();
        return;
    }

    uint32_t fallback = pick_probe_ip();
    if (!fallback)
        fallback = (192<<24)|(168<<16)|(1<<8)|255;

    if (udp_probe_server(fallback, 8080, &srv))
        test_http(srv.ip);
    else
        kprintf("[NET] could not find update server\n");
}

static int net_test_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    test_net();
    return 0;
}

static int ip_waiter_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    for (;;) {
        const net_cfg_t *cfg = ipv4_get_cfg();
        if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
            create_kernel_process("net_test", net_test_entry, 0, 0);
            break;
        }
        sleep(200);
    }
    return 0;
}

process_t* launch_net_process() {
    const net_cfg_t *cfg = ipv4_get_cfg();

    create_kernel_process("net_net", network_net_task_entry, 0, 0);
    create_kernel_process("arp_daemon", arp_daemon_entry, 0, 0);
    create_kernel_process("dhcp_daemon", dhcp_daemon_entry, 0, 0);
    create_kernel_process("dns_daemon", dns_deamon_entry, 0, 0);

    if (cfg && cfg->mode != NET_MODE_DISABLED && cfg->ip != 0) {
        create_kernel_process("net_test", net_test_entry, 0, 0);
        return NULL;
    }

    create_kernel_process("ip_waiter", ip_waiter_entry, 0, 0);
    return NULL;
}
