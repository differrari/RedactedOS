#include "http.h"
#include "std/string.h"
#include "tcp.h"
#include "syscalls/syscalls.h"
#include "ipv4.h"
#include "std/memfunctions.h"

string make_http_request(HTTPRequest request, char *domain, char *agent){
    //TODO: request instead of hardcoded GET
    return string_format("GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n\r\n",domain, agent);
}

sizedptr http_data_transfer(network_connection_ctx *dest, sizedptr payload, uint16_t port, tcp_data *data, uint8_t retry, uint32_t orig_seq, uint32_t orig_ack){
    if (retry == 5){
        printf("Exceeded max number of retries");
        return (sizedptr){0};
    }

    data->sequence = orig_seq;
    data->ack = orig_ack;
    data->flags = (1 << PSH_F) | (1 << ACK_F);

    data->payload = payload;

    tcp_send(port, dest, data);

    data->flags = (1 << ACK_F);

    uint8_t resp;
    do {
        resp = tcp_check_response(data, 0);
        if (resp == TCP_OK)
            break;
        if (resp == TCP_RESET)//We don't reset, we ignore irrelevant packets (or we could parse them tbh)
            continue;
        if (resp == TCP_RETRY)
            return http_data_transfer(dest, payload, port, data, retry+1, orig_seq, orig_ack);
    } while (1);

    data->flags = (1 << PSH_F) | (1 << ACK_F);

    sizedptr http_content;

    resp = tcp_check_response(data, &http_content);
    if (resp == TCP_RETRY){
        sleep(1000);
        return http_data_transfer(dest, payload, port, data, retry+1, orig_seq, orig_ack);
    } else if (resp == TCP_RESET){
        tcp_reset(port, dest, data);
        return (sizedptr){0};
    }

    data->payload = (sizedptr){0};

    data->flags = (1 << ACK_F);
    tcp_send(port, dest, data);

    return http_content;
}

sizedptr request_http_data(HTTPRequest request, network_connection_ctx *dest, uint16_t port){
    tcp_data data = (tcp_data){
        .window = UINT16_MAX,
    };

    printf("TCP Handshake");

    if (!tcp_handskake(dest, 8888, &data, 0)){
        printf("TCP Handshake Error");
        return (sizedptr){0};
    }

    string serverstr = ipv4_to_string(dest->ip);
    string req = make_http_request(request, serverstr.data, "redactedos/0.0.1");

    free(serverstr.data, serverstr.mem_length);

    printf("HTTP Request");

    //TODO: more chunked support

    sizedptr http_response = http_data_transfer(dest, (sizedptr){(uintptr_t)req.data, req.length}, port, &data, 0, data.sequence, data.ack);

    printf("TCP End");

    free(req.data, req.mem_length);

    if (!tcp_close(dest, 8888, &data, 0, data.sequence, data.ack)){
        printf("TCP Connnection not closed");
        return (sizedptr){0};
    }

    return http_response;
}

sizedptr http_get_payload(sizedptr header){
    if (header.ptr && header.size > 0){
        int start = strindex((char*)header.ptr, "\r\n\r\n");
        if (start < header.size){
            return (sizedptr){header.ptr + start + 4,header.size-start-4};    
        }
    } 
    return (sizedptr){0,0};
}

string http_get_chunked_payload(sizedptr chunk){
    //TODO: allow finding 0 to know when we're done reading the payload
    if (chunk.ptr && chunk.size > 0){
        int sizetrm = strindex((char*)chunk.ptr, "\r\n");
        uint64_t chunk_size = parse_hex_u64((char*)chunk.ptr,sizetrm);
        return string_ca_max((char*)(chunk.ptr + sizetrm + 2),chunk_size);
    } 
    return (string){0};
}