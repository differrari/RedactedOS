#include "net_proc.h" 
#include "kernel_processes/kprocess_loader.h"
#include "process/scheduler.h"
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
#include "networking/application_layer/http_webserver.h"
#include "networking/application_layer/dhcp_daemon.h"
#include "networking/application_layer/dns/dns_daemon.h"
#include "networking/application_layer/dns/mdns_responder.h"
#include "networking/application_layer/ntp.h"
#include "networking/application_layer/ntp_daemon.h"
#include "networking/application_layer/dhcpv6_daemon.h"

#include "exceptions/timer.h"

#define HTTP_PORT 80
#define PROBE_PORT 8080
#define PROBE_TIMEOUT_MS 2000
#define PROBE_INTERVAL_MS 50

static int udp_probe_server(uint32_t probe_ip, uint16_t probe_port, net_l4_endpoint *out_l4) {
    socket_handle_t sock = create_socket(PROTO_UDP, NULL);
    if (!sock) return 0;
    if (set_socket_option(sock, SOCK_OPT_BROADCAST_ALLOWED, NULL, 0) < 0) {
        close_socket(sock);
        return 0;
    }

    net_l4_endpoint dst;
    make_ep(&probe_ip, probe_port, IP_VER4, &dst);

    static const char greeting[] = "hello";
    if (send_to_socket(sock, &dst, greeting, sizeof(greeting)) < 0) {
        close_socket(sock);
        return 0;
    }

    char recv_buf[64];
    net_l4_endpoint src = (net_l4_endpoint){0};
    uint32_t waited = 0;
    int64_t recvd = 0;

    while (waited < PROBE_TIMEOUT_MS) {
        recvd = receive_from_socket(sock, recv_buf, sizeof(recv_buf), &src);
        if (recvd > 0) break;
        msleep(PROBE_INTERVAL_MS);
        waited += PROBE_INTERVAL_MS;
    }

    close_socket(sock);

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
            if (!ipv4_l3_is_ready(v4) || v4->is_localhost) continue;
            if (!ipv4_is_loopback(v4->ip)) return 1;
        }

        for (uint8_t j = 0; j < MAX_IPV6_PER_INTERFACE; j++) {
            l3_ipv6_interface_t *v6 = l2->l3_v6[j];
            if (!ipv6_l3_is_ready(v6) || v6->is_localhost) continue;
            if (!ipv6_is_loopback(v6->ip)) return 1;
        }
    }

    return 0;
}

static void run_http_server() {
    static const char HTTP_ROOT_BODY[] =
        "<!doctype html>\n"
        "<html><head><title>RedactedOS</title>\n"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">\n"
        "</head>\n"
        "<body>\n"
        "<h1>Hello, world!</h1>\n"
        "<h3>[Redacted]</h3>"
        "<p><img src=\"/assets/test.png\"></p>\n"
        "<p><a href=\"/assets/test.png\" download=\"test.png\">download test</a></p>\n"
        "</body></html>\n";

    static char HTML_404[] =
        "<h1>404 Regrettably, no such page exists in this realm</h1>\n"
        "<p>Im rather inclined to deduce that your page simply does not exist. Given the state of affairs, I dare say it's not altogether surprising, innit?</p>";

    static const HTTPRoute routes[] = {
        {
            .path = "/",
            .methods = HTTP_METHOD_MASK_GET,
            .flags = HTTP_ROUTE_HEAD_AS_GET,
            .kind = HTTP_ROUTE_STATIC,
            .as.response = {
                .status = HTTP_OK,
                .content_type = "text/html",
                .body = HTTP_ROOT_BODY,
                .body_len = sizeof(HTTP_ROOT_BODY) - 1
            }
        },
        {
            .path = "/assets/test.png",
            .methods = HTTP_METHOD_MASK_GET,
            .flags = HTTP_ROUTE_HEAD_AS_GET,
            .kind = HTTP_ROUTE_FILE,
            .as.file = {
                .fs_path = "/boot/redos/system/demo.red/resources/test.png",
                .content_type = "image/png",
                .cache_max_age_sec = 60
            }
        },
        {
            .path = "/favicon.ico",
            .methods = HTTP_METHOD_MASK_GET,
            .flags = HTTP_ROUTE_HEAD_AS_GET,
            .kind = HTTP_ROUTE_FILE,
            .as.file = {
                .fs_path = "/boot/redos/system/demo.red/resources/test.png",
                .content_type = "image/png",
                .cache_max_age_sec = 60
            }
        },
    };

    HTTPWebServerConfig config = {0};
    config.port = HTTP_PORT;
    config.backlog = 4;
    config.routes = routes;
    config.route_count = N_ARR(routes);
    config.not_found = HTTP_WEB_HTML_RESPONSE(HTTP_NOT_FOUND, HTML_404);
    config.mdns_instance = "RedactedOS";
    config.mdns_type = "http";
    config.mdns_proto = "tcp";
    config.mdns_txt = "path=/";

    http_webserver_run(&config);
    stop_current_process(0);
}

static void test_http(const net_l4_endpoint* ep) {
    if (!ep) return;
    if (ep->ver == IP_VER4) {
        uint32_t ip_u32;
        memcpy(&ip_u32, ep->ip, 4);
        char ip_str[16];
        ipv4_to_string(ip_u32, ip_str);

        print("[HTTP] GET %s:%i", ip_str, HTTP_PORT);
    }

    http_client_handle_t cli = http_client_create(NULL, NULL);
    if (!cli) return;

    net_l4_endpoint e = {0};
    e.ver = ep->ver;
    e.port = HTTP_PORT;
    if (e.ver == IP_VER4) memcpy(e.ip, ep->ip, 4);
    else if (e.ver == IP_VER6) memcpy(e.ip, ep->ip, 16);
    else {
        http_client_destroy(cli);
        return;
    }

    if (http_client_connect_endpoint(cli, &e) < 0) {
        http_client_destroy(cli);
        return;
    }

    HTTPRequestMsg req = (HTTPRequestMsg){0};
    req.method = HTTP_METHOD_GET;
    req.version = HTTP_VERSION_11;
    req.path = string_from_const("/");
    req.headers_common.fields.connection = string_from_const("close");

    HTTPResponseMsg resp = http_client_send_request(cli, &req);

    if ((int)resp.status_code < 0) print("[HTTP] request FAIL status=%i", (int)resp.status_code);
    else if (resp.body.data && resp.body.length) {
        char *body_str = (char*)zalloc(resp.body.length + 1);
        if (body_str) {
            memcpy(body_str, (void*)resp.body.data, resp.body.length);
            body_str[resp.body.length] = '\0';
            print("[HTTP] %i %i bytes of body", resp.status_code, resp.body.length);
            print("%s", body_str);
            release(body_str);
        }
    }

    http_client_close(cli);
    http_client_destroy(cli);

    http_response_free(&resp);
    http_request_free(&req);
}

static int ntp(int argc, char* argv[]) {
    (void)argc; (void)argv;
    if (!ntp_is_running()) {
        print("[TIME] starting NTP...");
        create_kernel_process("ntpd", ntp_daemon_entry, 0, 0);
        uint32_t waited = 0;
        const uint32_t step = 200;
        const uint32_t timeout = 10000;
        while (!timer_is_synchronised() && waited < timeout) {
            if ((waited % 1000) == 0) print("[TIME] waiting NTP sync...");
            msleep(step);
            waited += step;
        }
    }
    timer_set_timezone_minutes(120);
    print("[TIME]timezone offset %i minutes", (int32_t)timer_get_timezone_minutes());

    DateTime now_dt_utc, now_dt_loc;
    if (timer_now_datetime(&now_dt_utc, 0)) {
        char s[20];
        timer_datetime_to_string(&now_dt_utc, s, sizeof(s));
        print("[TIME] UTC: %s", s);
    }
    if (timer_now_datetime(&now_dt_loc, 1)) {
        char s[20];
        timer_datetime_to_string(&now_dt_loc, s, sizeof(s));
        print("[TIME] LOCAL: %s (TZ %i min)", s, (int32_t)timer_get_timezone_minutes());
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
            if (!ipv4_l3_is_ready(ifv4) || ifv4->is_localhost) continue;
            if (ipv4_is_loopback(ifv4->ip) || !ifv4->mask) continue;

            uint32_t probe_ip = ipv4_broadcast_calc(ifv4->ip, ifv4->mask);
            if (!probe_ip) continue;

            net_l4_endpoint srv = (net_l4_endpoint){0};
            if (udp_probe_server(probe_ip, PROBE_PORT, &srv)) {
                test_http(&srv);
                tested_any = 1;
            } 
        }
    }
    if (!tested_any) {
        net_l4_endpoint srv = (net_l4_endpoint){0};
        uint32_t fallback = (192<<24)|(168<<16)|(1<<8)|255;
        if (udp_probe_server(fallback, PROBE_PORT, &srv)) test_http(&srv);
    }

    run_http_server();
    return 0;
}

static int ip_waiter_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    while (!net_has_ready_address()) msleep(200);
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
        print("[NET] ip ready, starting net_test");
        create_kernel_process("net_test", net_test_entry, 0, 0);
    } else create_kernel_process("ip_waiter", ip_waiter_entry, 0, 0);

    return NULL;
}