#ifndef _DEVICE_ADDRESS_SPACE_H_
#define _DEVICE_ADDRESS_SPACE_H_

#include <array>
#include <filesystem>
#include <mutex>
#include <string>

class MemorySpace {
public:
    using Address = uint32_t;

    struct MemoryNotAvailable : public virtual std::runtime_error {
        MemoryNotAvailable() : std::runtime_error("") {}
    };

    virtual ~MemorySpace();

    Address allocate(size_t len, size_t align = 1);
    Address allocate_pages(size_t len);
    void free(Address addr, size_t len);
    void free_pages(Address addr, size_t len);

    virtual void read(Address addr, void* buf, size_t len);
    virtual void write(Address addr, const void* buf, size_t len);
    virtual void memset(Address addr, int c, size_t len);

    const void* get_raw_ptr(Address addr, size_t& len) const;

    void* get_map_base() const { return map_base; }
    size_t get_map_size() const { return map_size; }
    Address get_iova_base() const { return iova_base; }

protected:
    void* map_base;
    size_t map_size;
    Address iova_base;

    MemorySpace(Address iova_base = 0);

private:
    static constexpr size_t NR_HOLES = 512;

    struct hole {
        struct hole* h_next;
        Address h_base;
        Address h_len;
    };

    pthread_mutex_t* alloc_mutex;

    struct hole* hole;
    struct hole* hole_head;
    struct hole* free_slots;

    void delete_slot(struct hole* prev_ptr, struct hole* hp);
    void merge_hole(struct hole* hp);
};

class SharedMemorySpace : public MemorySpace {
public:
    explicit SharedMemorySpace(const std::filesystem::path& filename);

private:
    std::filesystem::path filename;
};

class VfioMemorySpace : public MemorySpace {
public:
    explicit VfioMemorySpace(Address iova_base, size_t map_size);

private:
    std::filesystem::path filename;
};

class BARMemorySpace : public MemorySpace {
public:
    explicit BARMemorySpace(void* base, size_t size);

    virtual void read(Address addr, void* buf, size_t len);
    virtual void write(Address addr, const void* buf, size_t len);
    virtual void memset(Address addr, int c, size_t len);
};

#endif
