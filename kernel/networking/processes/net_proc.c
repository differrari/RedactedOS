#include "net_proc.h"
#include "kernel_processes/kprocess_loader.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "std/memory.h"
#include "std/string.h"

#include "net/network_types.h"
#include "networking/network.h"
#include "networking/interface_manager.h"

#include "networking/link_layer/arp.h"

#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"

#include "networking/transport_layer/csocket_udp.h"

#include "networking/application_layer/csocket_http_client.h"
#include "networking/application_layer/csocket_http_server.h"
#include "networking/application_layer/dhcp_daemon.h"
#include "networking/application_layer/dns_daemon.h"
#include "networking/application_layer/dns.h"
#include "networking/application_layer/sntp_daemon.h"

#include "exceptions/timer.h"
#include "syscalls/syscalls.h"


static inline int ipv4_is_loopback_u32(uint32_t ip) {
    return ((ip & 0xFF000000u) == 0x7F000000u);
}

static uint32_t pick_probe_ip_v4(const l3_ipv4_interface_t *ifv4) {
    if (ifv4->ip && ifv4->mask) return ipv4_broadcast_calc(ifv4->ip, ifv4->mask);
    return 0;
}

static int udp_probe_server(uint32_t probe_ip, uint16_t probe_port, net_l4_endpoint *out_l4) {
    socket_handle_t sock = udp_socket_create(SOCK_ROLE_CLIENT, (uint16_t)get_current_proc_pid());
    if (!sock)
        return 0;

    net_l4_endpoint dst = (net_l4_endpoint){0};
    dst.ver = IP_VER4;
    memcpy(dst.ip, &probe_ip, 4);
    dst.port = probe_port;

    static const char greeting[] = "hello";
    if (socket_sendto_udp_ex(sock, DST_ENDPOINT, &dst, 0, greeting, sizeof(greeting)) < 0) {
        socket_close_udp(sock);
        socket_destroy_udp(sock);
        return 0;
    }

    char recv_buf[64];
    uint32_t waited = 0;
    const uint32_t TIMEOUT_MS = 2000;
    const uint32_t INTERVAL_MS = 50;
    int64_t recvd = 0;
    net_l4_endpoint src = (net_l4_endpoint){0};

    while (waited < TIMEOUT_MS) {
        recvd = socket_recvfrom_udp_ex(sock, recv_buf, sizeof(recv_buf), &src);
        if (recvd > 0)
            break;
        msleep(INTERVAL_MS);
        waited += INTERVAL_MS;
    }

    socket_close_udp(sock);
    socket_destroy_udp(sock);

    if (recvd <= 0) return 0;
    if (out_l4) *out_l4 = src;
    return 1;
}

static void free_request(HTTPRequestMsg *req) {
    if (req->path.mem_length)
        free_sized(req->path.data, req->path.mem_length);
    for (uint32_t i = 0; i < req->extra_header_count; i++) {
        HTTPHeader *h = &req->extra_headers[i];
        if (h->key.mem_length)
            free_sized(h->key.data, h->key.mem_length);
        if (h->value.mem_length)
            free_sized(h->value.data, h->value.mem_length);
    }
    if (req->extra_headers)
        free_sized(req->extra_headers, req->extra_header_count * sizeof(HTTPHeader));
    if (req->body.ptr && req->body.size)
        free_sized((void*)req->body.ptr, req->body.size);
}

static void run_http_server() {
    kprintf("[HTTP] server bootstrap");
    uint16_t pid = get_current_proc_pid();
    http_server_handle_t srv = http_server_create(pid);
    if (!srv) {
        stop_current_process(1);
        return;
    }
    struct SockBindSpec spec = {0};
    spec.kind = BIND_ANY;
    if (http_server_bind_ex(srv, &spec, 80) < 0) {
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

        HTTPResponseMsg res = (HTTPResponseMsg){0};

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

static void test_http(const net_l4_endpoint* ep) {
    if (ep->ver == IP_VER4) {
        uint32_t ip_u32;
        memcpy(&ip_u32, ep->ip, 4);
        char ip_str[16];
        ipv4_to_string(ip_u32, ip_str);

        kprintf("[HTTP] GET %s:80", ip_str);
    }

    uint16_t pid = get_current_proc_pid();
    http_client_handle_t cli = http_client_create(pid);
    if (!cli) {
        kprintf("[HTTP] http_client_create FAIL");
        return;
    }

    net_l4_endpoint e = {0};
    e.ver = IP_VER4;
    memcpy(e.ip, ep->ip, 4);
    e.port = 80;

    int rc = http_client_connect_ex(cli, DST_ENDPOINT, &e, 0);
    if (rc < 0) {
        http_client_destroy(cli);
        return;
    }

    HTTPRequestMsg req = (HTTPRequestMsg){0};
    req.method = HTTP_METHOD_GET;
    req.path = string_from_const("/");
    req.headers_common.connection = string_from_const("close");

    HTTPResponseMsg resp = http_client_send_request(cli, &req);

    //free(req.path.data, req.path.mem_length);
    //free(req.headers_common.connection.data, req.headers_common.connection.mem_length);

    if ((int)resp.status_code < 0) {
        kprintf("[HTTP] request FAIL status=%i", (int)resp.status_code);
        http_client_close(cli);
        http_client_destroy(cli);
        return;
    }

    if (resp.body.ptr && resp.body.size > 0) {
        char *body_str = (char*)malloc(resp.body.size + 1);
        if (body_str) {
            memcpy(body_str, (void*)resp.body.ptr, resp.body.size);
            body_str[resp.body.size] = '\0';
            kprintf("[HTTP] %i %i bytes of body", resp.status_code, resp.body.size);
            kprintf("%s", body_str);
            free_sized(body_str, resp.body.size + 1);
        }
    }

    http_client_close(cli);
    http_client_destroy(cli);

    if (resp.reason.data && resp.reason.mem_length)
        free_sized(resp.reason.data, resp.reason.mem_length);

    for (uint32_t i = 0; i < resp.extra_header_count; i++) {
        HTTPHeader *h = &resp.extra_headers[i];
        if (h->key.mem_length)
            free_sized(h->key.data, h->key.mem_length);
        if (h->value.mem_length)
            free_sized(h->value.data, h->value.mem_length);
    }

    if (resp.extra_headers)
        free_sized(resp.extra_headers, resp.extra_header_count * sizeof(HTTPHeader));
}


static void print_info_for_ifv4(const l3_ipv4_interface_t* ifv4) {
    if (!ifv4 || !ifv4->ip) return;
    if (ifv4->is_localhost) return;
    if (ipv4_is_loopback_u32(ifv4->ip)) return;
    char ip_str[16];
    char mask_str[16];
    char gw_str[16];
    ipv4_to_string(ifv4->ip, ip_str);
    ipv4_to_string(ifv4->mask, mask_str);
    ipv4_to_string(ifv4->gw, gw_str);
    kprintf("[NET] IF l3_id=%u IP: %s MASK: %s GW: %s", (unsigned)ifv4->l3_id, ip_str, mask_str, gw_str);
}

static int ifv4_is_ready_nonlocal(const l3_ipv4_interface_t* ifv4) {
    if (!ifv4) return 0;
    if (ifv4->mode == IPV4_CFG_DISABLED) return 0;
    if (!ifv4->ip) return 0;
    if (ifv4->is_localhost) return 0;
    if (ipv4_is_loopback_u32(ifv4->ip)) return 0;
    return 1;
}

static int any_ipv4_ready(void) {
    uint8_t n_if = l2_interface_count();
    for (uint8_t i = 0; i < n_if; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
            l3_ipv4_interface_t* ifv4 = l2->l3_v4[j];
            if (ifv4_is_ready_nonlocal(ifv4)) return 1;
        }
    }
    return 0;
}

static void print_info() {
    network_dump_interfaces();
    uint8_t n_if = l2_interface_count();
    for (uint8_t i = 0; i < n_if; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
            l3_ipv4_interface_t* ifv4 = l2->l3_v4[j];
            if (!ifv4_is_ready_nonlocal(ifv4)) continue;
            print_info_for_ifv4(ifv4);
        }
    }
    if (!sntp_is_running()) {
        kprintf("[TIME] starting SNTP...");
        create_kernel_process("sntpd", sntp_daemon_entry, 0, 0);
        uint32_t waited = 0;
        const uint32_t step = 200;
        const uint32_t timeout = 10000;
        while (!timer_is_synchronised() && waited < timeout) {
            if ((waited % 1000) == 0) kprintf("[TIME] waiting SNTP sync...");
            msleep(step);
            waited += step;

        }
        if (!timer_is_synchronised()) kprintf("[TIME] SNTP sync timeout, continuing");
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

static void test_net_for_interface(l3_ipv4_interface_t* ifv4) {
    if (!ifv4_is_ready_nonlocal(ifv4)) return;
    print_info_for_ifv4(ifv4);
    uint32_t probe_ip = pick_probe_ip_v4(ifv4);
    if (!probe_ip) return;
    char probe_str[16];
    ipv4_to_string(probe_ip, probe_str);
    kprintf("[NET] probing %s (l3_id=%u)", probe_str, (unsigned)ifv4->l3_id);
    net_l4_endpoint srv = (net_l4_endpoint){0};
    if (udp_probe_server(probe_ip, 8080, &srv)) {
        test_http(&srv);
    } else {
        kprintf("[NET] no UDP responder at %s:8080 (l3_id=%u)", probe_str, (unsigned)ifv4->l3_id);
    }
}

static void test_net() {
    print_info();
    uint8_t n_if = l2_interface_count();
    int tested_any = 0;
    for (uint8_t i = 0; i < n_if; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
            l3_ipv4_interface_t* ifv4 = l2->l3_v4[j];
            if (!ifv4_is_ready_nonlocal(ifv4)) continue;
            test_net_for_interface(ifv4);
            tested_any = 1;
        }
    }
    run_http_server();
    if (!tested_any) {
        net_l4_endpoint srv = (net_l4_endpoint){0};
        uint32_t fallback = (192<<24)|(168<<16)|(1<<8)|255;
        if (udp_probe_server(fallback, 8080, &srv))
            test_http(&srv);
        else
            kprintf("[NET] could not find update server");
    }
}

static int net_test_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    test_net();
    return 0;
}

static int ip_waiter_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    uint32_t waited = 0;
    while (!any_ipv4_ready()) {
        if ((waited % 1000) == 0) kprintf("[NET] ip_waiter: waiting for ipv4...");
        msleep(200);
        waited += 200;
    }
    create_kernel_process("net_test", net_test_entry, 0, 0);
    return 0;
}

process_t* launch_net_process() {
    create_kernel_process("net_net", network_net_task_entry, 0, 0);
    create_kernel_process("arp_daemon", arp_daemon_entry, 0, 0);
    create_kernel_process("dhcp_daemon", dhcp_daemon_entry, 0, 0);

    create_kernel_process("dns_daemon", dns_deamon_entry, 0, 0);

    if (any_ipv4_ready()) {
        kprintf("[NET] ipv4 ready, starting net_test");
        create_kernel_process("net_test", net_test_entry, 0, 0);
        return NULL;
    }

    create_kernel_process("ip_waiter", ip_waiter_entry, 0, 0);
    return NULL;
}
