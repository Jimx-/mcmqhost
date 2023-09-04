#ifndef _LIBUNVME_CAPI_H_
#define _LIBUNVME_CAPI_H_

#include <stdint.h>

typedef uint64_t storpu_handle_t;
#define INVALID_STORPU_HANDLE ((storpu_handle_t)-1)

struct storpu_tablescan {
    storpu_handle_t handle;
    unsigned long buf;
    size_t buf_size;
};

struct storpu_scankey;

#ifdef __cplusplus
extern "C"
{
#endif

    void unvme_init_driver(unsigned int num_workers, const char* group,
                           const char* device_id);
    void unvme_shutdown_driver(void);

    void unvme_set_queue(unsigned int qid);

    int unvme_read(unsigned int nsid, char* buf, size_t size, loff_t offset);

    void storpu_create_context(const char* library);
    void storpu_destroy_context(void);

    storpu_handle_t storpu_open_relation(int relid);
    void storpu_close_relation(storpu_handle_t rel);

    struct storpu_tablescan* storpu_table_beginscan(storpu_handle_t rel,
                                                    struct storpu_scankey* skey,
                                                    int num_skeys);
    size_t storpu_table_getnext(struct storpu_tablescan* scan, char* buf,
                                size_t buf_size);
    void storpu_table_endscan(struct storpu_tablescan* scan);

#ifdef __cplusplus
}
#endif

#endif
