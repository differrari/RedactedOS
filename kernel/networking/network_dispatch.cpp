#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"
#include "memory/page_allocator.h"
#include "net/link_layer/eth.h"
#include "port_manager.h"
#include "net/network_types.h"
#include "net/link_layer/arp.h"
#include "net/application_layer/dhcp_daemon.h"
#include "std/memfunctions.h"
#include "console/kio.h"
#include "types.h"
#include "process/scheduler.h"
#include "std/string.h"

#define QUEUE_CAPACITY 1024


//TODO: takes a lot of time to responde to every request. smt is creating a variable delay in receiving and trasmission somewhere
extern void      sleep(uint64_t ms);
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size); 

static uint16_t g_net_pid = 0xFFFF;

NetworkDispatch::NetworkDispatch() : ports(UINT16_MAX + 1),
    driver(nullptr),
    tx_queue(QUEUE_CAPACITY),
    rx_queue(QUEUE_CAPACITY)
{
    for (uint32_t i = 0; i <= UINT16_MAX; ++i)
        ports[i] = UINT16_MAX;

    memset(local_mac.mac, 0, sizeof(local_mac.mac));
}

bool NetworkDispatch::init()
{
    driver = VirtioNetDriver::try_init();
    if (!driver) return false;
    driver->get_mac(&local_mac);
    return true;
}

void NetworkDispatch::handle_download_interrupt()
{
    if (!driver) return;

    sizedptr raw = driver->handle_receive_packet();
    if (!raw.ptr) return;

    sizedptr frame = make_copy(raw);
    free_frame(raw);

    if (frame.ptr && frame.size >= sizeof(eth_hdr_t)) {
        if (!rx_queue.enqueue(frame))
            free_frame(frame);
    } else {
        free_frame(frame);
    }
}

void NetworkDispatch::handle_upload_interrupt()
{
    if (driver) driver->handle_sent_packet();
}

bool NetworkDispatch::enqueue_frame(const sizedptr &frame)
{
    if (frame.size == 0) {
        kprintf("[NetworkDispatch] drop zero‑length frame\n");
        return false;
    }

    sizedptr pkt = driver->allocate_packet(frame.size);
    if (!pkt.ptr) return false;

    void *dst = reinterpret_cast<void *>(pkt.ptr + driver->header_size);
    memcpy(dst, reinterpret_cast<const void *>(frame.ptr), frame.size);

    if (!tx_queue.enqueue(pkt)) {
        free_frame(pkt);
        return false;
    }
    return true;
}

void NetworkDispatch::net_task()
{
    for (;;) {
        bool did_work = false;
        sizedptr pkt;

        //rx
        if (!rx_queue.is_empty() && rx_queue.dequeue(pkt)) {
            did_work = true;
            eth_input(pkt.ptr, pkt.size);
            free_frame(pkt);
        }

        //tx
        if (!tx_queue.is_empty() && tx_queue.dequeue(pkt)) {
            did_work = true;
            driver->send_packet(pkt);
        }

        if (!did_work) sleep(10);
    }
}

bool NetworkDispatch::dequeue_packet_for(uint16_t pid, sizedptr *out)
{
    process_t *proc = get_proc_by_pid(pid);
    if (!proc || !out) return false;

    auto &buf = proc->packet_buffer;
    if (buf.read_index == buf.write_index) return false;

    sizedptr stored = buf.entries[buf.read_index];
    buf.read_index = (buf.read_index + 1) % PACKET_BUFFER_CAPACITY;

    void *dst = allocate_in_page(reinterpret_cast<void *>(get_current_heap()),
                                 stored.size, ALIGN_16B,
                                 get_current_privilege(), false);
    if (!dst) return false;

    memcpy(dst, reinterpret_cast<void *>(stored.ptr), stored.size);
    out->ptr = reinterpret_cast<uintptr_t>(dst);
    out->size = stored.size;

    free(reinterpret_cast<void *>(stored.ptr), stored.size);
    return true;
}

static sizedptr make_user_copy(const sizedptr &src)
{
    sizedptr out{0, 0};
    uintptr_t mem = malloc(src.size);
    if (!mem) return out;

    memcpy(reinterpret_cast<void *>(mem),
           reinterpret_cast<const void *>(src.ptr),
           src.size);

    out.ptr = mem;
    out.size = src.size;
    return out;
}

sizedptr NetworkDispatch::make_copy(const sizedptr &in)
{
    sizedptr out{0, 0};
    void *dst = allocate_in_page(reinterpret_cast<void *>(get_current_heap()),
                                 in.size, ALIGN_16B,
                                 get_current_privilege(), false);
    if (!dst) return out;

    memcpy(dst, reinterpret_cast<const void *>(in.ptr), in.size);
    out.ptr = reinterpret_cast<uintptr_t>(dst);
    out.size = in.size;
    return out;
}

void NetworkDispatch::free_frame(const sizedptr &f)
{
    if (f.ptr) free_sized(f);
}

void NetworkDispatch::set_net_pid(uint16_t pid) { g_net_pid = pid; }
uint16_t NetworkDispatch::get_net_pid() const { return g_net_pid; }
