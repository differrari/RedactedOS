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
#include "networking/link_layer/ndp.h"

#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"

#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/trans_utils.h"

#include "networking/application_layer/csocket_http_client.h"
#include "networking/application_layer/csocket_http_server.h"
#include "networking/application_layer/dhcp_daemon.h"
#include "networking/application_layer/dns/dns_daemon.h"
#include "networking/application_layer/dns/mdns_responder.h"
#include "networking/application_layer/ntp.h"
#include "networking/application_layer/ntp_daemon.h"
#include "networking/application_layer/dhcpv6_daemon.h"

#include "exceptions/timer.h"
#include "syscalls/syscalls.h"

#define HTTP_PORT 80
#define PROBE_PORT 8080
#define PROBE_TIMEOUT_MS 2000
#define PROBE_INTERVAL_MS 50

static int udp_probe_server(uint32_t probe_ip, uint16_t probe_port, net_l4_endpoint *out_l4) {
    socket_handle_t sock = {0};
    if (!create_socket(SOCKET_CLIENT, PROTO_UDP, NULL, &sock))
        return 0;

    net_l4_endpoint dst;
    make_ep(probe_ip, probe_port, IP_VER4, &dst);

    static const char greeting[] = "hello";
    if (send_to_socket(&sock, &dst, greeting, sizeof(greeting)) < 0) {
        close_socket(&sock);
        return 0;
    }

    char recv_buf[64];
    net_l4_endpoint src = (net_l4_endpoint){0};
    uint32_t waited = 0;
    int64_t recvd = 0;

    while (waited < PROBE_TIMEOUT_MS) {
        recvd = receive_from_socket(&sock, recv_buf, sizeof(recv_buf), &src);
        if (recvd > 0) break;
        msleep(PROBE_INTERVAL_MS);
        waited += PROBE_INTERVAL_MS;
    }

    close_socket(&sock);

    if (recvd <= 0) return 0;
    if (out_l4) *out_l4 = src;
    return 1;
}

static int net_has_ready_address(void) {
    uint8_t n_if = l2_interface_count();

    for (uint8_t i = 0; i < n_if; i++) {
        l2_interface_t *l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;

        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
            l3_ipv4_interface_t *v4 = l2->l3_v4[j];
            if (!v4 || v4->mode == IPV4_CFG_DISABLED || v4->is_localhost) continue;
            if (!ipv4_is_unspecified(v4->ip) && !ipv4_is_loopback(v4->ip)) return 1;
        }

        for (uint8_t j = 0; j < MAX_IPV6_PER_INTERFACE; j++) {
            l3_ipv6_interface_t *v6 = l2->l3_v6[j];
            if (!v6 || v6->cfg == IPV6_CFG_DISABLE || v6->is_localhost) continue;
            if (!ipv6_is_unspecified(v6->ip) && !ipv6_is_loopback(v6->ip) && v6->dad_state == IPV6_DAD_OK) return 1;
        }
    }

    return 0;
}

static void run_http_server() {
    SocketExtraOptions opt = {0};
    opt.debug_level = SOCK_DBG_ALL;
    opt.flags = SOCK_OPT_DEBUG;
    http_server_handle_t srv = http_server_create(&opt, NULL);
    if (!srv) {
        stop_current_process(1);
        return;
    }
    struct SockBindSpec spec = {0};
    spec.kind = BIND_ANY;
    if (http_server_bind(srv, &spec, HTTP_PORT) < 0) {
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

    mdns_register_service("RedactedOS", "http", "tcp", HTTP_PORT, "path=/");

    static const char HTML_ROOT[] =
        "<h1>Hello, world!</h1>\n"
        "<h3>[Redacted]</h3>";

    static const char HTML_404[] =
        "<h1>404 Regrettably, no such page exists in this realm</h1>\n"
        "<p>Im rather inclined to deduce that your page simply does not exist. Given the state of affairs, I dare say it's not altogether surprising, innit?</p>";

    while (1) {
        http_connection_handle_t conn = http_server_accept(srv);
        if (!conn){
            msleep(50);
            continue;
        }
        HTTPRequestMsg req = http_server_recv_request(srv, conn);
        int is_root = req.path.length == 1 && req.path.data[0] == '/';
        const char *body = is_root ? HTML_ROOT : HTML_404;
        uint32_t len = (uint32_t)strlen(body);

        HTTPResponseMsg res = (HTTPResponseMsg){0};
        res.status_code = is_root ? HTTP_OK : HTTP_NOT_FOUND;
        res.reason = string_from_const(is_root ? "OK" : "Not Found");
        res.headers_common.fields.content_length = len;
        res.headers_common.framing.has_content_length = 1;
        res.headers_common.fields.content_type = string_from_const("text/html");
        res.headers_common.fields.connection = string_from_const("close");
        res.body.ptr = (uintptr_t)body;
        res.body.size = len;

        http_server_send_response(srv, conn, &res);
        http_connection_close(conn);
        if (req.path.mem_length) string_free(req.path);
        http_headers_common_free(&req.headers_common);
        http_headers_extra_free(req.extra_headers, req.extra_header_count);
        if (req.body.ptr && req.body.size) release((void*)req.body.ptr);
    }
}

static void test_http(const net_l4_endpoint* ep) {
    if (!ep) return;
    if (ep->ver == IP_VER4) {
        uint32_t ip_u32;
        memcpy(&ip_u32, ep->ip, 4);
        char ip_str[16];
        ipv4_to_string(ip_u32, ip_str);

        kprintf("[HTTP] GET %s:%i", ip_str, HTTP_PORT);
    }

    http_client_handle_t cli = http_client_create(NULL, NULL);
    if (!cli) {
        kprintf("[HTTP] http_client_create FAIL");
        return;
    }

    net_l4_endpoint e = {0};
    e.ver = ep->ver;
    e.port = HTTP_PORT;
    if (e.ver == IP_VER4) memcpy(e.ip, ep->ip, 4);
    else if (e.ver == IP_VER6) memcpy(e.ip, ep->ip, 16);
    else {
        http_client_destroy(cli);
        return;
    }

    int32_t rc = http_client_connect_endpoint(cli, &e);
    if (rc < 0) {
        http_client_destroy(cli);
        return;
    }

    HTTPRequestMsg req = (HTTPRequestMsg){0};
    req.method = HTTP_METHOD_GET;
    req.version = HTTP_VERSION_11;
    req.path = string_from_const("/");
    req.headers_common.fields.connection = string_from_const("close");

    HTTPResponseMsg resp = http_client_send_request(cli, &req);

    if ((int)resp.status_code < 0) kprintf("[HTTP] request FAIL status=%i", (int)resp.status_code);
    else if (resp.body.ptr && resp.body.size) {
        char *body_str = (char*)zalloc(resp.body.size + 1);
        if (body_str) {
            memcpy(body_str, (void*)resp.body.ptr, resp.body.size);
            body_str[resp.body.size] = '\0';
            kprintf("[HTTP] %i %i bytes of body", resp.status_code, resp.body.size);
            kprintf("%s", body_str);
            release(body_str);
        }
    }

    http_client_close(cli);
    http_client_destroy(cli);

    if (resp.body.ptr && resp.body.size) release((void*)resp.body.ptr);
    if (resp.reason.mem_length) string_free(resp.reason);

    http_headers_common_free(&resp.headers_common);
    http_headers_extra_free(resp.extra_headers, resp.extra_header_count);
}

static int ntp(int argc, char* argv[]) {
    (void)argc; (void)argv;
    if (!ntp_is_running()) {
        kprintf("[TIME] starting NTP...");
        create_kernel_process("ntpd", ntp_daemon_entry, 0, 0);
        uint32_t waited = 0;
        const uint32_t step = 200;
        const uint32_t timeout = 10000;
        while (!timer_is_synchronised() && waited < timeout) {
            if ((waited % 1000) == 0) kprintf("[TIME] waiting NTP sync...");
            msleep(step);
            waited += step;

        }
        if (!timer_is_synchronised()) kprintf("[TIME] NTP sync timeout, continuing");
    }
    timer_set_timezone_minutes(120);
    kprintf("[TIME]timezone offset %i minutes", (int32_t)timer_get_timezone_minutes());

    DateTime now_dt_utc, now_dt_loc;
    if (timer_now_datetime(&now_dt_utc, 0)) {
        char s[20];
        timer_datetime_to_string(&now_dt_utc, s, sizeof(s));
        kprintf("[TIME] UTC: %s", s);
    }
    if (timer_now_datetime(&now_dt_loc, 1)) {
        char s[20];
        timer_datetime_to_string(&now_dt_loc, s, sizeof(s));
        kprintf("[TIME] LOCAL: %s (TZ %i min)", s, (int32_t)timer_get_timezone_minutes());
    }
    return 0;
}

static int net_test_entry(int argc, char *argv[]) {
    (void)argc; (void)argv;

    create_kernel_process("ntp", ntp, 0, NULL);
    uint8_t n_if = l2_interface_count();
    int tested_any = 0;
    for (uint8_t i = 0; i < n_if; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        for (uint8_t j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
            l3_ipv4_interface_t* ifv4 = l2->l3_v4[j];
            if (!ifv4 || ifv4->mode == IPV4_CFG_DISABLED || ifv4->is_localhost) continue;
            if (ipv4_is_unspecified(ifv4->ip) || ipv4_is_loopback(ifv4->ip) || !ifv4->mask) continue;

            uint32_t probe_ip = ipv4_broadcast_calc(ifv4->ip, ifv4->mask);
            if (!probe_ip) continue;

            char probe_str[16];
            ipv4_to_string(probe_ip, probe_str);
            kprintf("[NET] probing %s (l3_id=%u)", probe_str, (unsigned)ifv4->l3_id);

            net_l4_endpoint srv = (net_l4_endpoint){0};
            if (udp_probe_server(probe_ip, PROBE_PORT, &srv)) {
                test_http(&srv);
                tested_any = 1;
            } else kprintf("[NET] no UDP responder at %s:%i (l3_id=%u)", probe_str, PROBE_PORT, (unsigned)ifv4->l3_id);
        }
    }
    if (!tested_any) {
        net_l4_endpoint srv = (net_l4_endpoint){0};
        uint32_t fallback = (192<<24)|(168<<16)|(1<<8)|255;
        if (udp_probe_server(fallback, PROBE_PORT, &srv)) test_http(&srv);
        else kprintf("[NET] could not find update server");
    }

    run_http_server();
    return 0;
}

static int ip_waiter_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    uint32_t waited = 0;
    while (!net_has_ready_address()) {
        if ((waited % 1000) == 0) kprintf("[NET] ip_waiter: waiting for ip...");
        msleep(200);
        waited += 200;
    }
    create_kernel_process("net_test", net_test_entry, 0, 0);
    return 0;
}

process_t* launch_net_process() {
    create_kernel_process("net_net", network_net_task_entry, 0, 0);
    create_kernel_process("arp_daemon", arp_daemon_entry, 0, 0);
    create_kernel_process("ndp_daemon", ndp_daemon_entry, 0, 0);
    //create_kernel_process("ssdp_daemon", ssdp_daemon_entry, 0, 0);
    create_kernel_process("dhcp_daemon", dhcp_daemon_entry, 0, 0);
    create_kernel_process("dhcpv6_daemon", dhcpv6_daemon_entry, 0, 0);
    create_kernel_process("dns_daemon", dns_deamon_entry, 0, 0);
    
    if (net_has_ready_address()) {
        kprintf("[NET] ip ready, starting net_test");
        create_kernel_process("net_test", net_test_entry, 0, 0);
    } else create_kernel_process("ip_waiter", ip_waiter_entry, 0, 0);

    return NULL;
}