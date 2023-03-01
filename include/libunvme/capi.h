#ifndef _LIBUNVME_CAPI_H_
#define _LIBUNVME_CAPI_H_

#ifdef __cplusplus
extern "C"
{
#endif

    void unvme_init_driver(unsigned int num_workers, const char* group,
                           const char* device_id);
    void unvme_shutdown_driver();

    void unvme_set_queue(unsigned int qid);

    int unvme_read(unsigned int nsid, char* buf, size_t size, loff_t offset);

#ifdef __cplusplus
}
#endif

#endif
