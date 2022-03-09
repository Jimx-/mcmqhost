#include "nvme_driver.h"

#include "spdlog/spdlog.h"

#include <boost/endian/conversion.hpp>

namespace endian = boost::endian;

#define SQ_SIZE(nvmeq) ((nvmeq)->depth << ((nvmeq)->sqe_shift))
#define CQ_SIZE(nvmeq) ((nvmeq)->depth * sizeof(NVMeDriver::NVMeCompletion))

thread_local struct NVMeDriver::NVMeQueue* NVMeDriver::thread_io_queue =
    nullptr;

NVMeDriver::DeviceIOError::DeviceIOError(const char* msg)
    : std::runtime_error(msg)
{
    spdlog::error("NVMe device IO error: {}", msg);
}

NVMeDriver::NVMeStatus NVMeDriver::AsyncCommand::wait(NVMeResult* resp)
{
    NVMeStatus out_status;

    {
        std::unique_lock<std::mutex> lock(mutex);

        while (!completed)
            cv.wait(lock);

        if (resp) *resp = result;
        out_status = status;
    }

    driver->remove_async_command(id);
    return out_status;
}

NVMeDriver::NVMeDriver(unsigned ncpus, unsigned int io_queue_depth,
                       PCIeLink* link, MemorySpace* memory_space)
    : ncpus(ncpus), io_queue_depth(io_queue_depth), link(link),
      memory_space(memory_space), queue_count(0), online_queues(0)
{}

void NVMeDriver::set_thread_id(unsigned int thread_id)
{
    assert(thread_id < queues.size());
    thread_io_queue = queues[thread_id].get();
}

void NVMeDriver::start(const mcmq::SsdConfig& config)
{
    link->send_config(config);
    link->wait_for_device_ready();

    /* IO queues + admin queue */
    for (int i = 0; i < ncpus + 1; i++)
        queues.emplace_back(std::make_unique<NVMeQueue>());

    link->set_irq_handler([this](uint16_t vector) {
        NVMeQueue* nvmeq = queues[vector].get();
        nvme_irq(nvmeq);
    });

    reset();
}

void NVMeDriver::allocate_queue(unsigned qid, unsigned depth)
{
    auto& nvmeq = queues[qid];

    nvmeq->sqe_shift = qid ? NVME_NVM_IOSQES : NVME_ADM_SQES;
    nvmeq->depth = depth;

    nvmeq->sq_dma_addr = memory_space->allocate_pages(SQ_SIZE(nvmeq));
    nvmeq->cq_dma_addr = memory_space->allocate_pages(CQ_SIZE(nvmeq));

    nvmeq->qid = qid;
    nvmeq->q_db = NVME_REG_DBS + qid * 8 * db_stride;
    nvmeq->cq_phase = 1;
    nvmeq->cq_head = 0;
    queue_count++;

    spdlog::info("Allocate NVMe queue {} sq_addr={:#x} cq_addr={:#x} depth={}",
                 qid, nvmeq->sq_dma_addr, nvmeq->cq_dma_addr, depth);
}

void NVMeDriver::init_queue(unsigned qid)
{
    auto& nvmeq = queues[qid];

    nvmeq->sq_tail = 0;
    nvmeq->last_sq_tail = 0;
    nvmeq->cq_head = 0;
    nvmeq->q_db = NVME_REG_DBS + qid * 8 * db_stride;
    nvmeq->cq_phase = 1;
    memory_space->memset(nvmeq->cq_dma_addr, 0, CQ_SIZE(nvmeq));
    online_queues++;
}

void NVMeDriver::reset()
{
    ctrl_cap = link->readq(NVME_REG_CAP);

    queue_depth = std::min((int)(NVME_CAP_MQES(ctrl_cap) + 1), io_queue_depth);
    db_stride = 1 << NVME_CAP_STRIDE(ctrl_cap);

    spdlog::info("NVMe cap register queue_depth={} db_stride={}", queue_depth,
                 db_stride);

    setup_admin_queue();

    setup_io_queues();
}

void NVMeDriver::wait_ready(bool enabled)
{
    uint32_t csts, bit = enabled ? NVME_CSTS_RDY : 0;

    while (true) {
        csts = link->readl(NVME_REG_CSTS);

        if (csts == ~0) {
            throw DeviceIOError("Bad CSTS register value from device");
        }

        if ((csts & NVME_CSTS_RDY) == bit) break;
    }
}

void NVMeDriver::disable_controller()
{
    ctrl_config &= ~NVME_CC_SHN_MASK;
    ctrl_config &= ~NVME_CC_ENABLE;

    link->writel(NVME_REG_CC, ctrl_config);

    wait_ready(false);
}

void NVMeDriver::enable_controller()
{
    unsigned dev_page_min, page_shift = 12;

    ctrl_cap = link->readq(NVME_REG_CAP);
    dev_page_min = NVME_CAP_MPSMIN(ctrl_cap) + 12;

    if (page_shift < dev_page_min) {
        throw DeviceIOError("Unsupported page size from device");
    }

    ctrl_page_size = 1 << page_shift;

    ctrl_config = NVME_CC_CSS_NVM;
    ctrl_config |= (page_shift - 12) << NVME_CC_MPS_SHIFT;
    ctrl_config |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
    ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;
    ctrl_config |= NVME_CC_ENABLE;

    link->writel(NVME_REG_CC, ctrl_config);

    wait_ready(true);
}

NVMeDriver::AsyncCommand*
NVMeDriver::setup_async_command(AsyncCommandCallback&& callback)
{
    std::lock_guard<std::mutex> guard(command_mutex);
    auto id = command_id_counter.fetch_add(1);

    auto it = command_map.emplace(id, std::make_unique<AsyncCommand>()).first;
    auto& cmd = it->second;
    cmd->driver = this;
    cmd->id = id;
    cmd->completed = false;
    cmd->callback = callback;

    return it->second.get();
}

void NVMeDriver::remove_async_command(uint16_t command_id)
{
    std::lock_guard<std::mutex> guard(command_mutex);
    command_map.erase(command_id);
}

void NVMeDriver::write_sq_doorbell(NVMeQueue* nvmeq, bool write_sq)
{
    uint32_t tail;

    spdlog::trace("Writing SQ doorbell qid={} tail={}", nvmeq->qid,
                  nvmeq->sq_tail);

    if (!write_sq) {
        uint16_t next_tail = nvmeq->sq_tail + 1;

        if (next_tail == nvmeq->depth) next_tail = 0;
        if (next_tail != nvmeq->last_sq_tail) return;
    }

    tail = nvmeq->sq_tail;
    link->writel(nvmeq->q_db, tail);
    nvmeq->last_sq_tail = nvmeq->sq_tail;
}

void NVMeDriver::ring_cq_doorbell(NVMeQueue* nvmeq)
{
    uint32_t head = nvmeq->cq_head;
    link->writel(nvmeq->q_db + 4 * db_stride, head);
}

void NVMeDriver::submit_sq_command(NVMeQueue* nvmeq, struct nvme_command* cmd,
                                   bool write_sq)
{
    memory_space->write(nvmeq->sq_dma_addr +
                            (nvmeq->sq_tail << nvmeq->sqe_shift),
                        cmd, sizeof(*cmd));
    if (++nvmeq->sq_tail == nvmeq->depth) nvmeq->sq_tail = 0;
    write_sq_doorbell(nvmeq, write_sq);
}

NVMeDriver::NVMeStatus
NVMeDriver::submit_sync_command(NVMeQueue* nvmeq, struct nvme_command* cmd,
                                union nvme_completion::nvme_result* result)
{
    auto* acmd = setup_async_command({});

    cmd->common.command_id = acmd->id;

    submit_sq_command(nvmeq, cmd, true);

    return acmd->wait(result);
}

NVMeDriver::AsyncCommand*
NVMeDriver::submit_async_command(NVMeQueue* nvmeq, struct nvme_command* cmd,
                                 AsyncCommandCallback&& callback)
{
    auto* acmd = setup_async_command(std::move(callback));

    cmd->common.command_id = acmd->id;

    submit_sq_command(nvmeq, cmd, true);

    return acmd;
}

NVMeDriver::NVMeStatus NVMeDriver::nvme_features(uint8_t op, unsigned int fid,
                                                 unsigned int dword11,
                                                 uint32_t* result)
{
    struct nvme_command c;
    union nvme_completion::nvme_result res = {0};
    NVMeStatus status;

    memset(&c, 0, sizeof(c));
    c.features.opcode = op;
    c.features.fid = endian::native_to_little(fid);
    c.features.dword11 = endian::native_to_little(dword11);

    status = submit_sync_command(queues[0].get(), &c, &res);

    if (result) *result = endian::little_to_native(res.u32);
    return status;
}

NVMeDriver::NVMeStatus NVMeDriver::set_queue_count(int& count)
{
    uint32_t result;
    uint32_t q_count = (count - 1) | ((count - 1) << 16);
    int nioqs;
    NVMeDriver::NVMeStatus status;

    status = set_features(NVME_FEAT_NUM_QUEUES, q_count, &result);

    if (status > 0)
        count = 0;
    else {
        nioqs = std::min(result & 0xffff, result >> 16) + 1;
        count = std::min(count, nioqs);
    }

    return NVME_SC_SUCCESS;
}

void NVMeDriver::setup_admin_queue()
{
    disable_controller();

    allocate_queue(0, NVME_AQ_DEPTH);

    auto& nvmeq = queues.front();
    uint32_t aqa = nvmeq->depth - 1;
    aqa |= aqa << 16;

    uint64_t asq = nvmeq->sq_dma_addr;
    uint64_t acq = nvmeq->cq_dma_addr;

    link->writel((uint64_t)NVME_REG_AQA, aqa);
    link->writeq((uint64_t)NVME_REG_ASQ, asq);
    link->writeq((uint64_t)NVME_REG_ACQ, acq);

    enable_controller();

    init_queue(0);
}

void NVMeDriver::setup_io_queues()
{
    int nr_io_queues = queues.size() - 1;
    int max_qs;
    NVMeStatus status;

    status = set_queue_count(nr_io_queues);
    if (status || nr_io_queues == 0) {
        throw DeviceIOError("Failed to set queue count on device");
    }

    for (int i = queue_count; i <= nr_io_queues; i++) {
        allocate_queue(i, queue_depth);
    }

    max_qs = std::min(queue_count - 1, (size_t)nr_io_queues);

    for (int i = online_queues; i <= max_qs; i++) {
        NVMeStatus status;

        status = create_queue(queues[i].get(), i);
        if (status != NVME_SC_SUCCESS) break;
    }
}

NVMeDriver::NVMeStatus NVMeDriver::create_queue(NVMeQueue* nvmeq, unsigned qid)
{
    NVMeStatus status;

    status = create_cq(nvmeq, qid, qid);
    if (status) return status;

    status = create_sq(nvmeq, qid);
    if (status) return status;

    init_queue(qid);
    return 0;
}

NVMeDriver::NVMeStatus NVMeDriver::create_cq(NVMeQueue* nvmeq, unsigned qid,
                                             unsigned int vector)
{
    struct nvme_command c = {0};
    auto& adminq = queues.front();
    int flags = NVME_QUEUE_PHYS_CONTIG;

    memset(&c, 0, sizeof(c));
    c.create_cq.opcode = nvme_admin_create_cq;
    c.create_cq.prp1 = endian::native_to_little((uint64_t)nvmeq->cq_dma_addr);
    c.create_cq.cqid = endian::native_to_little(qid);
    c.create_cq.qsize = endian::native_to_little(nvmeq->depth - 1);
    c.create_cq.cq_flags = endian::native_to_little(flags);
    c.create_cq.irq_vector = endian::native_to_little(vector);

    return submit_sync_command(adminq.get(), &c, nullptr);
}

NVMeDriver::NVMeStatus NVMeDriver::create_sq(NVMeQueue* nvmeq, unsigned qid)
{
    struct nvme_command c = {0};
    auto& adminq = queues.front();
    int flags = NVME_QUEUE_PHYS_CONTIG;

    memset(&c, 0, sizeof(c));
    c.create_sq.opcode = nvme_admin_create_sq;
    c.create_sq.prp1 = endian::native_to_little((uint64_t)nvmeq->sq_dma_addr);
    c.create_sq.sqid = endian::native_to_little(qid);
    c.create_sq.qsize = endian::native_to_little(nvmeq->depth - 1);
    c.create_sq.sq_flags = endian::native_to_little(flags);
    c.create_sq.cqid = endian::native_to_little(qid);

    return submit_sync_command(adminq.get(), &c, nullptr);
}

bool NVMeDriver::cqe_pending(NVMeQueue* nvmeq)
{
    uint16_t status;
    struct nvme_completion* cqes = nullptr;

    memory_space->read(nvmeq->cq_dma_addr +
                           ((uintptr_t)&cqes[nvmeq->cq_head].status),
                       &status, sizeof(status));
    endian::little_to_native_inplace(status);
    return (status & 1) == nvmeq->cq_phase;
}

void NVMeDriver::handle_cqe(NVMeQueue* nvmeq, uint16_t idx)
{
    struct nvme_completion cqe;
    uint16_t command_id;

    memory_space->read(nvmeq->cq_dma_addr + (idx * sizeof(cqe)), &cqe,
                       sizeof(cqe));

    command_id = cqe.command_id;

    bool release_cmd = false;

    {
        std::lock_guard<std::mutex> guard(command_mutex);
        auto it = command_map.find(command_id);

        if (it == command_map.end()) {
            spdlog::error("Completion queue entry without command id={}",
                          command_id);
            return;
        }

        auto& cmd = it->second;
        {
            std::unique_lock<std::mutex> lock(cmd->mutex);

            auto status = endian::little_to_native(cqe.status) >> 1;
            if (cmd->callback) {
                cmd->callback(status, cqe.result);
                release_cmd = true;
            } else {
                cmd->status = status;
                cmd->result = cqe.result;
                cmd->completed = true;

                cmd->cv.notify_all();
            }
        }
    }

    if (release_cmd) remove_async_command(command_id);
}

void NVMeDriver::nvme_irq(NVMeQueue* nvmeq)
{
    int found = 0;

    while (cqe_pending(nvmeq)) {
        found++;
        handle_cqe(nvmeq, nvmeq->cq_head);
        nvmeq->update_cq_head();
    }

    if (found) ring_cq_doorbell(nvmeq);
}

NVMeDriver::AsyncCommand*
NVMeDriver::submit_rw_command(bool do_write, unsigned int nsid, loff_t pos,
                              size_t size, AsyncCommandCallback&& callback)
{
    uint16_t control = 0;
    uint32_t dsmgmt = 0;
    struct nvme_command cmd;

    spdlog::trace("Submitting {} command pos={} size={}",
                  do_write ? "write" : "read", pos, size);

    memset(&cmd, 0, sizeof(cmd));
    cmd.rw.opcode = (do_write ? nvme_cmd_write : nvme_cmd_read);
    cmd.rw.nsid = endian::native_to_little(nsid);
    cmd.rw.slba = endian::native_to_little(pos >> 9);
    cmd.rw.length = endian::native_to_little((size >> 9) - 1);

    cmd.rw.control = endian::native_to_little(control);
    cmd.rw.dsmgmt = endian::native_to_little(dsmgmt);

    return submit_async_command(thread_io_queue, &cmd, std::move(callback));
}

void NVMeDriver::read(unsigned int nsid, loff_t pos, size_t size)
{
    auto cmd = submit_rw_command(false, nsid, pos, size, {});
    auto status = cmd->wait(nullptr);
    if ((status & 0x7ff) == NVME_SC_SUCCESS) return;

    throw DeviceIOError("Read command error");
}

void NVMeDriver::write(unsigned int nsid, loff_t pos, size_t size)
{
    auto cmd = submit_rw_command(false, nsid, pos, size, {});
    auto status = cmd->wait(nullptr);
    if ((status & 0x7ff) == NVME_SC_SUCCESS) return;

    throw DeviceIOError("Read command error");
}
