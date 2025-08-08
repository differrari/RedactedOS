#pragma once
#include "types.h"
#include "std/std.hpp"
#include "drivers/net_driver.hpp"
#include "data_struct/queue.hpp"
#include "net/network_types.h"
#include "net/internet_layer/ipv4.h"

class NetworkDispatch {
public:
    NetworkDispatch();

    bool init();
    
    void handle_download_interrupt();
    void handle_upload_interrupt();
    bool enqueue_frame(const sizedptr&);
    void net_task();
    bool dequeue_packet_for(uint16_t, sizedptr*);

    void set_net_pid(uint16_t pid);
    uint16_t get_net_pid() const;


    const net_l2l3_endpoint& get_local_ep() const {
        static net_l2l3_endpoint ep; //TODO: locking/thread safe would be good
        ep = local_mac;
        ep.ip = ipv4_get_cfg()->ip;
        return ep;
    }


    NetDriver* driver_ptr() const { return driver; }
    uint16_t header_size() const { return driver ? driver->header_size : 0; }

private:
    static constexpr size_t QUEUE_CAPACITY = 1024;

    IndexMap<uint16_t> ports; //port pid map
    NetDriver* driver;
    net_l2l3_endpoint local_mac;

    Queue<sizedptr> tx_queue;
    Queue<sizedptr> rx_queue;

    sizedptr make_copy(const sizedptr&);
    void free_frame(const sizedptr&);
};
