#include "libunvme/nvme_driver.h"

#include "spdlog/spdlog.h"

#include <boost/endian/conversion.hpp>

#include <fstream>

namespace endian = boost::endian;

#define SQ_SIZE(nvmeq) ((nvmeq)->depth << ((nvmeq)->sqe_shift))
#define CQ_SIZE(nvmeq) ((nvmeq)->depth * sizeof(NVMeDriver::NVMeCompletion))

thread_local NVMeDriver::PNVMeQueue NVMeDriver::thread_io_queue = nullptr;

NVMeDriver::DeviceIOError::DeviceIOError(const char* msg)
    : std::runtime_error(msg)
{
    spdlog::error("NVMe device IO error: {}", msg);
}

NVMeDriver::NamespaceUnavailableError::NamespaceUnavailableError()
    : std::runtime_error("")
{
    spdlog::error("NVMe namespace unavailable");
}

NVMeDriver::NVMeStatus NVMeDriver::AsyncCommand::wait(NVMeResult* resp)
{
    NVMeStatus out_status;

    {
        std::unique_lock<MutexType> lock(mutex);

        while (!completed)
            cv.wait(lock);

        if (resp) *resp = result;
        out_status = status;
    }

    driver->remove_async_command(id);
    return out_status;
}

NVMeDriver::NVMeDriver(unsigned ncpus, unsigned int io_queue_depth,
                       PCIeLink* link, MemorySpace* memory_space,
                       bool use_dbbuf)
    :
#ifdef ENABLE_INTERPROCESS
      segment(boost::interprocess::open_or_create, "NVMeDriver", 512 << 20),
#endif
      ncpus(ncpus), io_queue_depth(io_queue_depth), link(link),
      memory_space(memory_space), queue_count(0), online_queues(0)
{
    if (use_dbbuf) {
        dbbuf_dbs = memory_space->allocate(0x1000);
        memory_space->memset(dbbuf_dbs, 0, 0x1000);
        spdlog::info("Allocated doorbell buffer at {:#x}", dbbuf_dbs);
    } else
        dbbuf_dbs = 0;

    bar4_mem = link->map_bar(4);

#ifdef ENABLE_INTERPROCESS
    queues = segment.construct<std::vector<UniquePtrType<NVMeQueue>>>(
        boost::interprocess::anonymous_instance)();
    command_mutex =
        segment.construct<MutexType>(boost::interprocess::anonymous_instance)();
    command_map = segment.construct<CommandMapType>(
        boost::interprocess::anonymous_instance)(10,
                                                 segment.get_segment_manager());
    command_id_counter = segment.construct<std::atomic<uint16_t>>(
        boost::interprocess::anonymous_instance)();
#else
    queues = new std::vector<UniquePtrType<NVMeQueue>>();
    command_mutex = new MutexType();
    command_map = new CommandMapType();
    command_id_counter = new std::atomic<uint16_t>();
#endif
}

NVMeDriver::~NVMeDriver()
{
    delete bar4_mem;

#ifdef ENABLE_INTERPROCESS
    segment.destroy_ptr(queues);
    segment.destroy_ptr(command_mutex);
    segment.destroy_ptr(command_map);
    segment.destroy_ptr(command_id_counter);
#else
    delete queues;
    delete command_mutex;
    delete command_map;
    delete command_id_counter;
#endif
}

void NVMeDriver::set_thread_id(unsigned int thread_id)
{
    assert(thread_id < queues->size());
    thread_io_queue = (*queues)[thread_id].get();
}

void NVMeDriver::start()
{
    link->wait_for_device_ready();

    /* IO queues + admin queue */
    for (int i = 0; i < ncpus + 1; i++)
        queues->emplace_back(make_unique<NVMeQueue>());

    link->set_irq_handler([this](uint16_t vector) {
        PNVMeQueue nvmeq = (*queues)[vector].get();
        nvme_irq(nvmeq);
    });

    reset();
}

void NVMeDriver::allocate_queue(unsigned qid, unsigned depth)
{
    auto& nvmeq = (*queues)[qid];

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
    auto& nvmeq = (*queues)[qid];

    nvmeq->sq_tail = 0;
    nvmeq->last_sq_tail = 0;
    nvmeq->cq_head = 0;
    nvmeq->q_db = NVME_REG_DBS + qid * 8 * db_stride;
    nvmeq->cq_phase = 1;

    if (dbbuf_dbs && qid) {
        nvmeq->dbbuf_sq_db = dbbuf_dbs + qid * 8 * db_stride;
        nvmeq->dbbuf_cq_db = dbbuf_dbs + qid * 8 * db_stride + 4 * db_stride;
    }

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

    identify_controller();

    setup_io_queues();

    if (dbbuf_dbs) {
        dbbuf_config();
    }
}

void NVMeDriver::check_status(int status)
{
    switch (status) {
    case NVME_SC_SUCCESS:
        return;
    case NVME_SC_NS_ID_UNAVAILABLE:
        throw NamespaceUnavailableError();
    default:
        throw DeviceIOError("NVMe internal error");
    }
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

void NVMeDriver::shutdown()
{
    ctrl_config &= ~NVME_CC_SHN_MASK;
    ctrl_config |= NVME_CC_SHN_NORMAL;

    link->writel(NVME_REG_CC, ctrl_config);
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

NVMeDriver::PAsyncCommand
NVMeDriver::setup_async_command(AsyncCommandCallback&& callback)
{
    std::lock_guard<MutexType> guard(*command_mutex);
    auto id = command_id_counter->fetch_add(1);

    auto it = command_map->emplace(id, make_unique<AsyncCommand>()).first;
    auto& cmd = it->second;
    cmd->driver = this;
    cmd->space = memory_space;
    cmd->id = id;
    cmd->completed = false;
    cmd->callback = callback;

    return it->second.get();
}

void NVMeDriver::remove_async_command(uint16_t command_id)
{
    std::lock_guard<MutexType> guard(*command_mutex);
    command_map->erase(command_id);
}

void NVMeDriver::setup_buffer(PAsyncCommand acmd, struct nvme_command* cmd,
                              MemorySpace::Address buf, size_t buflen)
{
    auto offset = buf % ctrl_page_size;

    if (offset + buflen <= ctrl_page_size * 2) {
        auto first_prp_len = ctrl_page_size - offset;

        cmd->common.dptr.prp1 = endian::native_to_little(buf);
        if (buflen > first_prp_len)
            cmd->common.dptr.prp2 =
                endian::native_to_little(buf + first_prp_len);

        return;
    }

    unsigned long prp1, prp2;

    prp1 = buf;

    buflen -= ctrl_page_size - offset;
    buf += ctrl_page_size - offset;

    auto prp_list = memory_space->allocate_pages(ctrl_page_size);
    acmd->prp_lists.push_back(prp_list);

    prp2 = prp_list;

    int i = 0;
    for (;;) {
        if (i == ctrl_page_size >> 3) {
            auto old_prp_list = prp_list;
            uint64_t last_entry;

            prp_list = memory_space->allocate_pages(ctrl_page_size);
            acmd->prp_lists.push_back(prp_list);

            memory_space->read(old_prp_list + (i - 1) * sizeof(uint64_t),
                               &last_entry, sizeof(last_entry));
            memory_space->write(prp_list, &last_entry, sizeof(last_entry));
            last_entry = endian::native_to_little(prp_list);
            memory_space->write(old_prp_list + (i - 1) * sizeof(uint64_t),
                                &last_entry, sizeof(last_entry));
        }

        uint64_t val = endian::native_to_little(buf);
        memory_space->write(prp_list + i * sizeof(uint64_t), &val, sizeof(val));
        i++;
        buf += ctrl_page_size;

        if (buflen <= ctrl_page_size) break;
        buflen -= ctrl_page_size;
    }

    cmd->common.dptr.prp1 = endian::native_to_little(prp1);
    cmd->common.dptr.prp2 = endian::native_to_little(prp2);
}

void NVMeDriver::write_sq_doorbell(PNVMeQueue nvmeq, bool write_sq)
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

    if (nvmeq->dbbuf_sq_db)
        memory_space->write(nvmeq->dbbuf_sq_db, &tail, sizeof(tail));
    else
        link->writel(nvmeq->q_db, tail);

    nvmeq->last_sq_tail = nvmeq->sq_tail;
}

void NVMeDriver::ring_cq_doorbell(PNVMeQueue nvmeq)
{
    uint32_t head = nvmeq->cq_head;

    if (nvmeq->dbbuf_cq_db)
        memory_space->write(nvmeq->dbbuf_cq_db, &head, sizeof(head));
    else
        link->writel(nvmeq->q_db + 4 * db_stride, head);
}

void NVMeDriver::submit_sq_command(PNVMeQueue nvmeq, struct nvme_command* cmd,
                                   bool write_sq)
{
    memory_space->write(nvmeq->sq_dma_addr +
                            (nvmeq->sq_tail << nvmeq->sqe_shift),
                        cmd, sizeof(*cmd));
    if (++nvmeq->sq_tail == nvmeq->depth) nvmeq->sq_tail = 0;
    write_sq_doorbell(nvmeq, write_sq);
}

NVMeDriver::NVMeStatus
NVMeDriver::submit_sync_command(PNVMeQueue nvmeq, struct nvme_command* cmd,
                                MemorySpace::Address buf, size_t buflen,
                                union nvme_completion::nvme_result* result)
{
    auto acmd = setup_async_command({});

    cmd->common.command_id = acmd->id;

    if (buflen) {
        setup_buffer(acmd, cmd, buf, buflen);
    }

    submit_sq_command(nvmeq, cmd, true);

    return acmd->wait(result);
}

NVMeDriver::PAsyncCommand
NVMeDriver::submit_async_command(PNVMeQueue nvmeq, struct nvme_command* cmd,
                                 MemorySpace::Address buf, size_t buflen,
                                 AsyncCommandCallback&& callback)
{
    auto acmd = setup_async_command(std::move(callback));

    cmd->common.command_id = acmd->id;

    if (buflen) {
        setup_buffer(acmd, cmd, buf, buflen);
    }

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

    status = submit_sync_command((*queues)[0].get(), &c, 0, 0, &res);

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

void NVMeDriver::dbbuf_config()
{
    struct nvme_command c;
    NVMeStatus status;

    memset(&c, 0, sizeof(c));
    c.common.opcode = nvme_admin_dbbuf;
    c.common.dptr.prp1 = endian::native_to_little(dbbuf_dbs);

    status = submit_sync_command((*queues)[0].get(), &c, 0, 0, nullptr);

    if (status) throw DeviceIOError("Failed to set dbbuf on device");
}

void NVMeDriver::setup_admin_queue()
{
    disable_controller();

    allocate_queue(0, NVME_AQ_DEPTH);

    auto& nvmeq = queues->front();
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
    int nr_io_queues = queues->size() - 1;
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

        status = create_queue((*queues)[i].get(), i);
        if (status != NVME_SC_SUCCESS) break;
    }
}

NVMeDriver::NVMeStatus NVMeDriver::create_queue(PNVMeQueue nvmeq, unsigned qid)
{
    NVMeStatus status;

    status = create_cq(nvmeq, qid, qid);
    if (status) return status;

    status = create_sq(nvmeq, qid);
    if (status) return status;

    init_queue(qid);
    return 0;
}

NVMeDriver::NVMeStatus NVMeDriver::create_cq(PNVMeQueue nvmeq, unsigned qid,
                                             unsigned int vector)
{
    struct nvme_command c = {0};
    auto& adminq = queues->front();
    int flags = NVME_QUEUE_PHYS_CONTIG;

    memset(&c, 0, sizeof(c));
    c.create_cq.opcode = nvme_admin_create_cq;
    c.create_cq.prp1 = endian::native_to_little((uint64_t)nvmeq->cq_dma_addr);
    c.create_cq.cqid = endian::native_to_little(qid);
    c.create_cq.qsize = endian::native_to_little(nvmeq->depth - 1);
    c.create_cq.cq_flags = endian::native_to_little(flags);
    c.create_cq.irq_vector = endian::native_to_little(vector);

    return submit_sync_command(adminq.get(), &c, 0, 0, nullptr);
}

NVMeDriver::NVMeStatus NVMeDriver::create_sq(PNVMeQueue nvmeq, unsigned qid)
{
    struct nvme_command c = {0};
    auto& adminq = queues->front();
    int flags = NVME_QUEUE_PHYS_CONTIG;

    memset(&c, 0, sizeof(c));
    c.create_sq.opcode = nvme_admin_create_sq;
    c.create_sq.prp1 = endian::native_to_little((uint64_t)nvmeq->sq_dma_addr);
    c.create_sq.sqid = endian::native_to_little(qid);
    c.create_sq.qsize = endian::native_to_little(nvmeq->depth - 1);
    c.create_sq.sq_flags = endian::native_to_little(flags);
    c.create_sq.cqid = endian::native_to_little(qid);

    return submit_sync_command(adminq.get(), &c, 0, 0, nullptr);
}

NVMeDriver::NVMeStatus NVMeDriver::identify_controller()
{
    struct nvme_command c = {0};
    auto& adminq = queues->front();
    MemorySpace::Address id_buf;
    int status;

    memset(&c, 0, sizeof(c));
    c.identify.opcode = nvme_admin_identify;
    c.identify.cns = NVME_ID_CNS_CTRL;

    id_buf = memory_space->allocate_pages(sizeof(struct nvme_id_ctrl));

    status = submit_sync_command(adminq.get(), &c, id_buf,
                                 sizeof(struct nvme_id_ctrl), nullptr);

    memory_space->free(id_buf, sizeof(struct nvme_id_ctrl));

    return status;
}

bool NVMeDriver::cqe_pending(PNVMeQueue nvmeq)
{
    uint16_t status;
    struct nvme_completion* cqes = nullptr;

    assert(nvmeq->cq_head < nvmeq->depth);
    memory_space->read(nvmeq->cq_dma_addr +
                           ((uintptr_t)&cqes[nvmeq->cq_head].status),
                       &status, sizeof(status));
    endian::little_to_native_inplace(status);
    return (status & 1) == nvmeq->cq_phase;
}

void NVMeDriver::handle_cqe(PNVMeQueue nvmeq, uint16_t idx)
{
    struct nvme_completion cqe;
    uint16_t command_id;

    assert(idx < nvmeq->depth);
    memory_space->read(nvmeq->cq_dma_addr + (idx * sizeof(cqe)), &cqe,
                       sizeof(cqe));

    command_id = cqe.command_id;

    bool release_cmd = false;

    {
        std::lock_guard<MutexType> guard(*command_mutex);
        auto it = command_map->find(command_id);

        if (it == command_map->end()) {
            spdlog::error("Completion queue entry without command id={}",
                          command_id);
            return;
        }

        auto& cmd = it->second;
        {
            std::unique_lock<AsyncCommand::MutexType> lock(cmd->mutex);

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

void NVMeDriver::nvme_irq(PNVMeQueue nvmeq)
{
    int found = 0;

    while (cqe_pending(nvmeq)) {
        found++;
        handle_cqe(nvmeq, nvmeq->cq_head);
        nvmeq->update_cq_head();
    }

    if (found) ring_cq_doorbell(nvmeq);
}

NVMeDriver::PAsyncCommand
NVMeDriver::submit_rw_command(bool do_write, unsigned int nsid, loff_t pos,
                              MemorySpace::Address buf, size_t size,
                              AsyncCommandCallback&& callback)
{
    uint16_t control = 0;
    uint32_t dsmgmt = 0;
    struct nvme_command cmd;

    spdlog::trace("Submitting {} command pos={} size={}",
                  do_write ? "write" : "read", pos, size);

    memset(&cmd, 0, sizeof(cmd));
    cmd.rw.opcode = (do_write ? nvme_cmd_write : nvme_cmd_read);
    cmd.rw.nsid = endian::native_to_little(nsid);
    cmd.rw.slba = endian::native_to_little(pos >> 12);
    cmd.rw.length = endian::native_to_little((size >> 12) - 1);

    cmd.rw.control = endian::native_to_little(control);
    cmd.rw.dsmgmt = endian::native_to_little(dsmgmt);

    return submit_async_command(thread_io_queue, &cmd, buf, size,
                                std::move(callback));
}

NVMeDriver::PAsyncCommand
NVMeDriver::submit_flush_command(unsigned int nsid,
                                 AsyncCommandCallback&& callback)
{
    struct nvme_command cmd;

    spdlog::trace("Submitting flush command nsid={}", nsid);

    memset(&cmd, 0, sizeof(cmd));
    cmd.rw.opcode = nvme_cmd_flush;
    cmd.rw.nsid = endian::native_to_little(nsid);

    return submit_async_command(thread_io_queue, &cmd, 0, 0,
                                std::move(callback));
}

void NVMeDriver::read(unsigned int nsid, loff_t pos, MemorySpace::Address buf,
                      size_t size)
{
    auto cmd = submit_rw_command(false, nsid, pos, buf, size, {});
    auto status = cmd->wait(nullptr);
    if ((status & 0x7ff) == NVME_SC_SUCCESS) return;

    throw DeviceIOError("Read command error");
}

void NVMeDriver::write(unsigned int nsid, loff_t pos, MemorySpace::Address buf,
                       size_t size)
{
    auto cmd = submit_rw_command(true, nsid, pos, buf, size, {});
    auto status = cmd->wait(nullptr);
    if ((status & 0x7ff) == NVME_SC_SUCCESS) return;

    throw DeviceIOError("Write command error");
}

void NVMeDriver::flush(unsigned int nsid)
{
    auto cmd = submit_flush_command(nsid, {});
    auto status = cmd->wait(nullptr);
    if ((status & 0x7ff) == NVME_SC_SUCCESS) return;

    throw DeviceIOError("Flush command error");
}

uint32_t NVMeDriver::create_context(const std::filesystem::path& filename)
{
    std::ifstream ifs(filename, std::ios::binary);

    ifs.seekg(0, std::ios::end);
    size_t length = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    size_t alloc_length = (length + 7) & ~0x7;
    auto buffer = std::make_unique<uint8_t[]>(alloc_length);

    ifs.read((char*)buffer.get(), length);

    auto dev_buf = bar4_mem->allocate(alloc_length);
    bar4_mem->write(dev_buf, buffer.get(), alloc_length);

    struct nvme_command c = {0};
    union nvme_completion::nvme_result res;
    auto& adminq = queues->front();

    memset(&c, 0, sizeof(c));
    c.common.opcode = nvme_admin_storpu_create_context;
    c.common.dptr.prp1 = endian::native_to_little((uint64_t)dev_buf);

    auto status = submit_sync_command(adminq.get(), &c, 0, 0, &res);

    bar4_mem->free(dev_buf, alloc_length);

    if ((status & 0x7ff) != NVME_SC_SUCCESS)
        throw DeviceIOError("Create context error");

    return endian::little_to_native(res.u32);
}

void NVMeDriver::delete_context(unsigned int cid)
{
    struct nvme_command c = {0};
    union nvme_completion::nvme_result res;
    auto& adminq = queues->front();

    memset(&c, 0, sizeof(c));
    c.common.opcode = nvme_admin_storpu_delete_context;
    c.common.cdw10 = cid;

    auto status = submit_sync_command(adminq.get(), &c, 0, 0, &res);

    if ((status & 0x7ff) != NVME_SC_SUCCESS)
        throw DeviceIOError("Delete context error");
}

NVMeDriver::PAsyncCommand
NVMeDriver::submit_invoke_command(unsigned int cid, MemorySpace::Address entry,
                                  unsigned long arg,
                                  AsyncCommandCallback&& callback)
{
    struct nvme_command cmd;

    spdlog::trace("Submitting invoke command cid={} entry={} arg={}", cid,
                  entry, arg);

    memset(&cmd, 0, sizeof(cmd));
    cmd.storpu_invoke.opcode = nvme_cmd_storpu_invoke;
    cmd.storpu_invoke.nsid = endian::native_to_little(0);
    cmd.storpu_invoke.entry = endian::native_to_little((uint64_t)entry);
    cmd.storpu_invoke.arg = endian::native_to_little((uint64_t)arg);
    cmd.storpu_invoke.cid = endian::native_to_little((uint32_t)cid);

    return submit_async_command(thread_io_queue, &cmd, 0, 0,
                                std::move(callback));
}

unsigned long NVMeDriver::invoke_function(unsigned int cid,
                                          MemorySpace::Address entry,
                                          unsigned long arg)
{
    auto cmd = submit_invoke_command(cid, entry, arg, {});
    union nvme_completion::nvme_result res;
    auto status = cmd->wait(&res);

    if ((status & 0x7ff) != NVME_SC_SUCCESS)
        throw DeviceIOError("Read command error");

    return (unsigned long)endian::little_to_native(res.u64);
}

NVMeDriver::NVMeStatus
NVMeDriver::submit_ns_mgmt(unsigned int nsid, int sel,
                           MemorySpace::Address buffer, size_t size,
                           union nvme_completion::nvme_result* res)
{
    struct nvme_command c = {0};
    auto& adminq = queues->front();
    int status;

    memset(&c, 0, sizeof(c));
    c.common.opcode = nvme_admin_ns_mgmt;
    c.common.nsid = nsid;
    c.common.cdw10 = sel;

    status = submit_sync_command(adminq.get(), &c, buffer, size, res);

    return status;
}

unsigned int NVMeDriver::create_namespace(size_t size_bytes)
{
    struct nvme_id_ns* id_ns;
    MemorySpace::Address buffer;
    union nvme_completion::nvme_result res;
    int status;

    id_ns = new struct nvme_id_ns;
    memset(id_ns, 0, sizeof(*id_ns));
    id_ns->nsze = size_bytes >> 12;
    id_ns->ncap = size_bytes >> 12;

    buffer = memory_space->allocate_pages(sizeof(*id_ns));
    memory_space->write(buffer, id_ns, sizeof(*id_ns));

    status = submit_ns_mgmt(0, 0, buffer, sizeof(*id_ns), &res);

    memory_space->free(buffer, sizeof(*id_ns));
    delete id_ns;

    check_status(status & 0x7ff);

    return res.u32;
}

void NVMeDriver::delete_namespace(unsigned int nsid)
{
    int status;

    status = submit_ns_mgmt(nsid, 1, 0, 0, nullptr);

    check_status(status & 0x7ff);
}

NVMeDriver::NVMeStatus NVMeDriver::submit_ns_attach(unsigned int nsid, int sel,
                                                    MemorySpace::Address buffer,
                                                    size_t size)
{
    struct nvme_command c = {0};
    auto& adminq = queues->front();
    int status;

    memset(&c, 0, sizeof(c));
    c.common.opcode = nvme_admin_ns_attach;
    c.common.nsid = nsid;
    c.common.cdw10 = sel;

    status = submit_sync_command(adminq.get(), &c, buffer, size, nullptr);

    return status;
}

void NVMeDriver::attach_namespace(unsigned int nsid)
{
    uint16_t* ctrl_list;
    MemorySpace::Address buffer;
    union nvme_completion::nvme_result res;
    int status;

    ctrl_list = new uint16_t[0x800];
    memset(ctrl_list, 0, 0x1000);
    ctrl_list[0] = 0x1;

    buffer = memory_space->allocate_pages(0x1000);
    memory_space->write(buffer, ctrl_list, 0x1000);

    status = submit_ns_attach(nsid, 0, buffer, 0x1000);

    memory_space->free(buffer, 0x1000);
    delete[] ctrl_list;

    check_status(status & 0x7ff);
}

void NVMeDriver::detach_namespace(unsigned int nsid)
{
    uint16_t* ctrl_list;
    MemorySpace::Address buffer;
    union nvme_completion::nvme_result res;
    int status;

    ctrl_list = new uint16_t[0x800];
    memset(ctrl_list, 0, 0x1000);
    ctrl_list[0] = 0x1;

    buffer = memory_space->allocate_pages(0x1000);
    memory_space->write(buffer, ctrl_list, 0x1000);

    status = submit_ns_attach(nsid, 1, buffer, 0x1000);

    memory_space->free(buffer, 0x1000);
    delete[] ctrl_list;

    check_status(status & 0x7ff);
}
