//
// Created by ctyi on 7/12/18.
//

#include <string>
#include <malloc.h>
#include "rdma.h"
#include "../common/interprocess_t.h"

#define DEBUGON 1
#define RDMA_MAX_CONN 20

static int dev_port_id=0;
static struct ibv_context* ib_ctx=nullptr;
static uint8_t device_id=0;
static uint16_t port_lid=0;
static union ibv_gid RoCE_gid;
static struct ibv_pd * ibv_pd=nullptr;
struct ibv_mr* buf_mr=nullptr;


static void *MR_ptr=nullptr;
static uint32_t MR_rkey;



static std::string link_layer_str(uint8_t link_layer) {
    switch (link_layer) {
        case IBV_LINK_LAYER_UNSPECIFIED:
            return "[Unspecified]";
        case IBV_LINK_LAYER_INFINIBAND:
            return "[InfiniBand]";
        case IBV_LINK_LAYER_ETHERNET:
            return "[Ethernet]";
        default:
            return "[Invalid]";
    }
}


//always use the first port
static void enum_dev()
{

    // Get the device list
    int num_devices = 0;
    int ports_to_discover=0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    assert(dev_list != nullptr);

    DEBUG("%d RDMA NIC detected", num_devices);
    // Traverse the device list

    for (int dev_i = 0; dev_i < num_devices; dev_i++) {
        ib_ctx = ibv_open_device(dev_list[dev_i]);
        assert(ib_ctx != nullptr);

        struct ibv_device_attr device_attr;
        memset(&device_attr, 0, sizeof(device_attr));
        if (ibv_query_device(ib_ctx, &device_attr) != 0) {
            FATAL("Fail to query device %d");
        }

        for (uint8_t port_i = 1; port_i <= device_attr.phys_port_cnt; port_i++) {
            // Count this port only if it is enabled
            struct ibv_port_attr port_attr;
            if (ibv_query_port(ib_ctx, port_i, &port_attr) != 0) {
                FATAL("Failed to query port %d on dev %d", port_i, dev_i);
            }

            if (port_attr.phys_state != IBV_PORT_ACTIVE &&
                port_attr.phys_state != IBV_PORT_ACTIVE_DEFER) {
                continue;
            }

            if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
                FATAL(
                        "Transport type required is RoCE but port link layer is %s"
                        ,link_layer_str(port_attr.link_layer).c_str());
            }

                DEBUG("RDMA  NIC bind to device %d, port %d. Name %s.\n",
                       dev_i, port_i, dev_list[dev_i]->name);

                device_id = dev_i;
                dev_port_id = port_i;
                port_lid = port_attr.lid;

                // Resolve and cache the ibv_gid struct for RoCE
                    int ret = ibv_query_gid(ib_ctx, dev_port_id, 0, &RoCE_gid);
                    assert(ret == 0);

                return;

        }

        // Thank you Mario, but our port is in another device
        if (ibv_close_device(ib_ctx) != 0) {
            FATAL("Failed to close dev %d", dev_i);
        }
    }

    // If we are here, port resolution has failed
    assert(ib_ctx == nullptr);
    FATAL("Failed to enumerate device");
}
void rdma_init()
{
    //several things
    //1. Init a large buffer
    //2. Allocate PD
    //3. enumerate device and get dev id

    //enumerate device
    enum_dev();
    //allocate pd
    ibv_pd = ibv_alloc_pd(ib_ctx);
    if (ibv_pd == nullptr)
        FATAL("Failed to create Protected Domain for RDMA");

    //allocate a large buffer
    MR_ptr = memalign(4096, (size_t)RDMA_MAX_CONN * interprocess_t::get_sharedmem_size());
    if (MR_ptr == nullptr)
        FATAL("Failed to create a large buffer");
    //reg it to NIC MR

    int ib_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                   IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    buf_mr = ibv_reg_mr(ibv_pd, MR_ptr,(size_t)RDMA_MAX_CONN * interprocess_t::get_sharedmem_size(),ib_flags);
    if (buf_mr == nullptr)
        FATAL("Failed to reg MR for RDMA");

    DEBUG("RDMA Init finished!");
}

#undef DEBUGON