//
// Created by ctyi on 7/30/18.
//

#include <string>
#include "rdma.h"

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


#undef DEBUGON
#define DEBUGON 1

#define RDMA_CONFIG_NAME "rdma_config.txt"
static bool isRDMAnameGet=false;

//always use the first port
int enum_dev(rdma_pack *p)
{
    static char RDMA_DEV_NAME[100];
    // Get the device list
    int num_devices = 0;
    int ports_to_discover=0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (dev_list == nullptr) {
        DEBUG("WARN: no RDMA NIC detected");
        return -1;
    }

    DEBUG("%d RDMA NIC detected", num_devices);
    // Traverse the device list
    FILE *rdma_config;
    if (!isRDMAnameGet) {
        rdma_config = fopen(RDMA_CONFIG_NAME, "r");
        if (rdma_config == nullptr)
            FATAL("RDMA config failed to open");
        fscanf(rdma_config, "%s", RDMA_DEV_NAME);
        isRDMAnameGet = true;
    }
    for (int dev_i = 0; dev_i < num_devices; dev_i++) {
        p->ib_ctx = ibv_open_device(dev_list[dev_i]);
        if (p->ib_ctx == nullptr) {
            FATAL("Fail to open device %d", dev_i);
        }

        struct ibv_device_attr device_attr;
        memset(&device_attr, 0, sizeof(device_attr));
        if (ibv_query_device(p->ib_ctx, &device_attr) != 0) {
            FATAL("Fail to query device %d", dev_i);
        }
        if (strcmp(RDMA_DEV_NAME, dev_list[dev_i]->name) != 0)
            continue;

        for (uint8_t port_i = 1; port_i <= device_attr.phys_port_cnt; port_i++) {
            // Count this port only if it is enabled
            struct ibv_port_attr port_attr;
            if (ibv_query_port(p->ib_ctx, port_i, &port_attr) != 0) {
                FATAL("Failed to query port %d on dev %d", port_i, dev_i);
            }

            if (port_attr.phys_state != IBV_PORT_ACTIVE &&
                port_attr.phys_state != IBV_PORT_ACTIVE_DEFER) {
                continue;
            }

            /*if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
                FATAL(
                        "Transport type required is RoCE but port link layer is %s"
                        ,link_layer_str(port_attr.link_layer).c_str());
            }*/
            DEBUG("RDMA  NIC bind to device %d, port %d. Name %s.",
                  dev_i, port_i, dev_list[dev_i]->name);

            p->device_id = dev_i;
            p->dev_port_id = port_i;
            p->port_lid = port_attr.lid;

            // Resolve and cache the ibv_gid struct for RoCE
            int ret = ibv_query_gid(p->ib_ctx, p->dev_port_id, 0, &(p->RoCE_gid));
            assert(ret == 0);

            return 0;
        }

        // Thank you Mario, but our port is in another device
        if (ibv_close_device(p->ib_ctx) != 0) {
            FATAL("Failed to close dev %d", dev_i);
        }
    }

    // If we are here, port resolution has failed
    assert(p->ib_ctx == nullptr);
    DEBUG("Failed to enumerate device");
    return -1;
}

ibv_qp * rdma_create_qp(ibv_cq* send_cq, ibv_cq *recv_cq, const rdma_pack * rdma_context)
{
    ibv_qp_init_attr myqp_attr;
    memset(&myqp_attr, 0, sizeof(struct ibv_qp_init_attr));
    myqp_attr.send_cq = send_cq;
    myqp_attr.recv_cq = recv_cq;
    myqp_attr.qp_type = IBV_QPT_RC;
    myqp_attr.cap.max_send_wr = QPSQDepth;
    myqp_attr.cap.max_recv_wr = QPRQDepth;
    myqp_attr.cap.max_send_sge = 1;
    myqp_attr.cap.max_recv_sge = 1;
    myqp_attr.cap.max_inline_data = QPMaxInlineData;
    ibv_qp *qp;
    DEBUG("Starting create QP");
    if (rdma_context->ibv_pd == nullptr)
        FATAL("PD nullptr");
    if (recv_cq == nullptr)
        FATAL("RECV CQ nullptr");
    qp=ibv_create_qp(rdma_context->ibv_pd, &myqp_attr);
    DEBUG("QP created inside");
    if (qp == nullptr)
        FATAL("Failed to create QP, %s", strerror(errno));
    ibv_qp_attr myqp_stateupdate_attr;
    memset(&myqp_stateupdate_attr, 0, sizeof(myqp_stateupdate_attr));
    myqp_stateupdate_attr.qp_state = IBV_QPS_INIT;
    myqp_stateupdate_attr.pkey_index = 0;
    myqp_stateupdate_attr.port_num = rdma_context->dev_port_id;
    myqp_stateupdate_attr.qp_access_flags =
            IBV_ACCESS_REMOTE_READ |
            IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_ATOMIC;
    if (ibv_modify_qp(qp, &myqp_stateupdate_attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        FATAL("Failed to set QP to init");
    return qp;
}

void rdma_connect_remote_qp(ibv_qp *qp, const rdma_pack * rdma_context, const qp_info_t * remote_qp_info)
{

    ibv_qp_attr myqp_stateupdate_attr;
    //change state to RTR
    memset(&myqp_stateupdate_attr, 0, sizeof(myqp_stateupdate_attr));
    myqp_stateupdate_attr.qp_state = IBV_QPS_RTR;
    myqp_stateupdate_attr.path_mtu = IBV_MTU_4096;
    myqp_stateupdate_attr.dest_qp_num = remote_qp_info->qpn;
    myqp_stateupdate_attr.rq_psn = 3185;

    myqp_stateupdate_attr.ah_attr.is_global = 1;
    myqp_stateupdate_attr.ah_attr.dlid = remote_qp_info->port_lid;
    myqp_stateupdate_attr.ah_attr.sl = 0;
    myqp_stateupdate_attr.ah_attr.src_path_bits = 0;
    myqp_stateupdate_attr.ah_attr.port_num = rdma_context->dev_port_id;  // Local port!

    auto& grh = myqp_stateupdate_attr.ah_attr.grh;
    grh.dgid.global.interface_id = remote_qp_info->RoCE_gid.global.interface_id;
    grh.dgid.global.subnet_prefix = remote_qp_info->RoCE_gid.global.subnet_prefix;
    grh.sgid_index = 0;
    grh.hop_limit = 1;

    myqp_stateupdate_attr.max_dest_rd_atomic = 16;
    myqp_stateupdate_attr.min_rnr_timer = 12;
    int rtr_flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(qp, &myqp_stateupdate_attr, rtr_flags)) {
        FATAL("Failed to modify QP from INIT to RTR");
    }

    //set QP to RTS
    memset(&myqp_stateupdate_attr, 0, sizeof(myqp_stateupdate_attr));
    myqp_stateupdate_attr.qp_state = IBV_QPS_RTS;
    myqp_stateupdate_attr.sq_psn = 3185;

    int rts_flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                    IBV_QP_MAX_QP_RD_ATOMIC;
    myqp_stateupdate_attr.timeout = 14;
    myqp_stateupdate_attr.retry_cnt = 7;
    myqp_stateupdate_attr.rnr_retry = 7;
    myqp_stateupdate_attr.max_rd_atomic = 16;
    myqp_stateupdate_attr.max_dest_rd_atomic = 16;
    rts_flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                 IBV_QP_MAX_QP_RD_ATOMIC;


    if (ibv_modify_qp(qp, &myqp_stateupdate_attr, rts_flags)) {
        FATAL("Failed to modify QP from RTR to RTS");
    }
}

void post_rdma_write(volatile void * ele, uintptr_t remote_addr, uint32_t lkey, uint32_t rkey, ibv_qp * qp, ibv_cq * cq, size_t len)
{

    static unsigned int wqe_cnt=0;
    struct ibv_send_wr wr, *bad_send_wr;
    struct ibv_sge sgl;
    struct ibv_wc wc;

    int32_t ret;
    do
    {
        ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0)
            FATAL("Poll CQ Err %s", strerror(ret));
    } while (ret > 0);

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sgl;

    wr.send_flags = 0;


    sgl.addr = (uint64_t) ele;
    sgl.length = len;
    sgl.lkey = lkey;


    wr.wr.rdma.remote_addr = (uint64_t)remote_addr;
    wr.wr.rdma.rkey = rkey;

    /*
     * Caveat: You must regularly request CQE, otherwise CPU
     * won't know whether WQE finished, finally lead to ENOMEM because
     * Send Queue used up.
     */
    ++wqe_cnt;
    if (wqe_cnt % 64 == 0)
        wr.send_flags |= IBV_SEND_SIGNALED;


    ret = ibv_post_send(qp, &wr, &bad_send_wr);
    if (ret != 0) {
        FATAL("wrong ret %d", ret);
        return;
    }
    return;
}

void RDMA_flow_ctl_t::post_rdma_write(volatile void * ele, uintptr_t remote_addr, uint32_t lkey, uint32_t rkey,
                     ibv_qp * qp, size_t len, bool signaled)
{

    struct ibv_send_wr wr, *bad_send_wr;
    struct ibv_sge sgl;

    int32_t ret;

    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.num_sge = 1;
    wr.next = nullptr;
    wr.sg_list = &sgl;

    wr.send_flags = 0;


    sgl.addr = (uint64_t) ele;
    sgl.length = len;
    sgl.lkey = lkey;


    wr.wr.rdma.remote_addr = (uint64_t)remote_addr;
    wr.wr.rdma.rkey = rkey;

    /*
     * Caveat: You must regularly request CQE, otherwise CPU
     * won't know whether WQE finished, finally lead to ENOMEM because
     * Send Queue used up.
     */
    if (signaled)
        wr.send_flags |= IBV_SEND_SIGNALED;


    ret = ibv_post_send(qp, &wr, &bad_send_wr);
    if (ret != 0) {
        FATAL("wrong ret %d", ret);
        return;
    }
}

RDMA_flow_ctl_t::RDMA_flow_ctl_t(uint32_t _sq_depth, uint32_t _lkey, uint32_t _rkey, ibv_qp *_qp, ibv_cq *_send_cq):
    entry_lst(),
    avail_sq_depth(_sq_depth),
    num_req_sent(0),
    lkey(_lkey),
    rkey(_rkey),
    qp(_qp),
    send_cq(_send_cq)
{}

void RDMA_flow_ctl_t::reg(unpushed_data_t * entry)
{
    entry_lst.push_back(entry);
}

void RDMA_flow_ctl_t::sync()
{
    int32_t cq_ret;
    do
    {
        struct ibv_wc wc;
        cq_ret = ibv_poll_cq(send_cq, 1, &wc);
        avail_sq_depth += SEND_PER_SIGNAL;
        if (cq_ret < 0)
            FATAL("Poll CQ Err %s", strerror(cq_ret));
    } while (cq_ret > 0);


    unpushed_data_t::mem_buffer_t ret;
    for (auto entry : entry_lst)
    {
        if (avail_sq_depth == 0) break;
        do {
            ret = entry->get();
            if (ret.isexist) {
                ++num_req_sent;
                bool issignal(false);
                if (num_req_sent % SEND_PER_SIGNAL == 0)
                    issignal = true;
                post_rdma_write(ret.localptr,ret.remoteptr, lkey, rkey, qp, ret.size, issignal);
            }
        } while (ret.isexist);
    }
}

void RDMA_flow_ctl_t::direct_rdma_write(void * local_addr, uintptr_t remote_addr, unsigned long size)
{
    int32_t cq_ret;
    do
    {
        struct ibv_wc wc;
        cq_ret = ibv_poll_cq(send_cq, 1, &wc);
        avail_sq_depth += SEND_PER_SIGNAL;
        if (cq_ret < 0)
            FATAL("Poll CQ Err %s", strerror(cq_ret));
    } while (cq_ret > 0);

    ++num_req_sent;
    bool issignal(false);
    if (num_req_sent % SEND_PER_SIGNAL == 0)
        issignal = true;
    post_rdma_write(local_addr, remote_addr, lkey, rkey, qp, size, issignal);
}
