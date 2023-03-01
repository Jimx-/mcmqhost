#include "libunvme/memory_space.h"

#include "spdlog/spdlog.h"

#include <fcntl.h>
#include <sys/mman.h>

namespace fs = std::filesystem;

#define roundup(x, align) \
    (((x) % align == 0) ? (x) : (((x) + align) - ((x) % align)))

MemorySpace::MemorySpace(Address iova_base) : iova_base(iova_base)
{
    struct hole* hp;

    pthread_mutexattr_t attrmutex;
    pthread_mutexattr_init(&attrmutex);
    pthread_mutexattr_setpshared(&attrmutex, PTHREAD_PROCESS_SHARED);
    alloc_mutex = (pthread_mutex_t*)::mmap(NULL, sizeof(*alloc_mutex),
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pthread_mutex_init(alloc_mutex, &attrmutex);
    pthread_mutexattr_destroy(&attrmutex);

    hole = (struct hole*)::mmap(NULL, sizeof(struct hole) * NR_HOLES,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    for (hp = &hole[0]; hp < &hole[NR_HOLES]; hp++) {
        hp->h_next = hp + 1;
        hp->h_base = 0;
        hp->h_len = 0;
    }
    hole[NR_HOLES - 1].h_next = NULL;
    hole_head = NULL;
    free_slots = &hole[0];
}

MemorySpace::~MemorySpace()
{
    if (hole) munmap(hole, sizeof(struct hole) * NR_HOLES);
}

void MemorySpace::delete_slot(struct hole* prev_ptr, struct hole* hp)
{
    if (hp == hole_head)
        hole_head = hp->h_next;
    else
        prev_ptr->h_next = hp->h_next;

    hp->h_next = free_slots;
    hp->h_base = hp->h_len = 0;
    free_slots = hp;
}

void MemorySpace::merge_hole(struct hole* hp)
{
    struct hole* next_ptr;

    if ((next_ptr = hp->h_next) == NULL) return;
    if (hp->h_base + hp->h_len == next_ptr->h_base) {
        hp->h_len += next_ptr->h_len;
        delete_slot(hp, next_ptr);
    } else {
        hp = next_ptr;
    }

    if ((next_ptr = hp->h_next) == NULL) return;
    if (hp->h_base + hp->h_len == next_ptr->h_base) {
        hp->h_len += next_ptr->h_len;
        delete_slot(hp, next_ptr);
    }
}

MemorySpace::Address MemorySpace::allocate(size_t len, size_t align)
{
    struct hole *hp, *prev_ptr;
    Address old_base;

    pthread_mutex_lock(alloc_mutex);

    prev_ptr = NULL;
    hp = hole_head;
    while (hp != NULL) {
        size_t alignment = 0;
        if (hp->h_base % align != 0) alignment = align - (hp->h_base % align);
        if (hp->h_len >= len + alignment) {
            old_base = hp->h_base + alignment;
            hp->h_base += len + alignment;
            hp->h_len -= (len + alignment);
            if (prev_ptr && prev_ptr->h_base + prev_ptr->h_len == old_base)
                prev_ptr->h_len += alignment;

            if (hp->h_len == 0) delete_slot(prev_ptr, hp);

            pthread_mutex_unlock(alloc_mutex);
            return old_base;
        }

        prev_ptr = hp;
        hp = hp->h_next;
    }

    pthread_mutex_unlock(alloc_mutex);
    throw MemoryNotAvailable();
}

MemorySpace::Address MemorySpace::allocate_pages(size_t len)
{
    return allocate(roundup(len, 0x1000), 0x1000);
}

void MemorySpace::free(Address addr, size_t len)
{
    struct hole *hp, *new_ptr, *prev_ptr;

    if (len == 0) return;

    pthread_mutex_lock(alloc_mutex);

    if ((new_ptr = free_slots) == NULL) {
        pthread_mutex_unlock(alloc_mutex);
        spdlog::error("Memory space hole table full");
        abort();
    }

    new_ptr->h_base = addr;
    new_ptr->h_len = len;
    free_slots = new_ptr->h_next;
    hp = hole_head;

    if (hp == NULL || addr <= hp->h_base) {
        new_ptr->h_next = hp;
        hole_head = new_ptr;
        merge_hole(new_ptr);
        pthread_mutex_unlock(alloc_mutex);
        return;
    }

    prev_ptr = NULL;
    while (hp != NULL && addr > hp->h_base) {
        prev_ptr = hp;
        hp = hp->h_next;
    }

    new_ptr->h_next = prev_ptr->h_next;
    prev_ptr->h_next = new_ptr;
    merge_hole(prev_ptr);
    pthread_mutex_unlock(alloc_mutex);
}

void MemorySpace::free_pages(Address addr, size_t len)
{
    free(addr, roundup(len, 0x1000));
}

void MemorySpace::read(Address addr, void* buf, size_t len)
{
    spdlog::trace("MemorySpace::read({:#x}, {}, {})", addr, buf, len);

    addr -= iova_base;
    assert(addr < map_size && addr + len <= map_size);

    switch (len) {
    case 4:
        *(uint32_t*)buf = *(uint32_t*)((char*)map_base + addr);
        break;
    case 8:
        *(uint64_t*)buf = *(uint64_t*)((char*)map_base + addr);
        break;
    default:
        ::memcpy(buf, (char*)map_base + addr, len);
        break;
    }
}

void MemorySpace::write(Address addr, const void* buf, size_t len)
{
    spdlog::trace("MemorySpace::write({:#x}, {}, {})", addr, buf, len);

    addr -= iova_base;
    assert(addr < map_size && addr + len <= map_size);

    switch (len) {
    case 4:
        *(uint32_t*)((char*)map_base + addr) = *(uint32_t*)buf;
        break;
    case 8:
        *(uint64_t*)((char*)map_base + addr) = *(uint64_t*)buf;
        break;
    default:
        ::memcpy((char*)map_base + addr, buf, len);
        break;
    }
}

void MemorySpace::memset(Address addr, int c, size_t len)
{
    spdlog::trace("MemorySpace::memset({:#x}, {}, {})", addr, c, len);

    addr -= iova_base;
    assert(addr < map_size && addr + len <= map_size);
    ::memset((char*)map_base + addr, c, len);
}

const void* MemorySpace::get_raw_ptr(Address addr, size_t& len) const
{
    addr -= iova_base;

    if (addr >= map_size) return nullptr;

    len = std::min(len, map_size - addr);
    return (char*)map_base + addr;
}

SharedMemorySpace::SharedMemorySpace(const fs::path& filename)
    : MemorySpace(0), filename(filename)
{
    auto file_size = fs::file_size(filename);

    int fd = ::open(filename.c_str(), O_RDWR);
    if (fd == -1) {
        spdlog::error("Error opening shared memory file: {}",
                      ::strerror(errno));
        throw std::runtime_error("");
    }

    void* base =
        ::mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        spdlog::error("Error mapping shared memory file: {}",
                      ::strerror(errno));
        throw std::runtime_error("");
    }

    map_base = base;
    map_size = file_size;

    spdlog::info("Mapped shared memory file base={} size={}MB", map_base,
                 map_size >> 20);

    free(0x1000, file_size - 0x1000);
}

VfioMemorySpace::VfioMemorySpace(Address iova_base, size_t size)
    : MemorySpace(iova_base)
{
    void* base = ::mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        spdlog::error("Error mapping shared memory file: {}",
                      ::strerror(errno));
        throw std::runtime_error("");
    }

    map_base = base;
    map_size = size;

    spdlog::info("Mapped DMA memory base={} size={}MB", map_base,
                 map_size >> 20);

    free(iova_base, iova_base + map_size);
}

BARMemorySpace::BARMemorySpace(void* base, size_t size) : MemorySpace(0)
{
    map_base = base;
    map_size = size;

    spdlog::info("Mapped BAR memory base={} size={}MB", map_base,
                 map_size >> 20);

    free(0, map_size);
}

void BARMemorySpace::read(Address addr, void* buf, size_t len)
{
    spdlog::trace("BARMemorySpace::read({:#x}, {}, {})", addr, buf, len);

    addr -= iova_base;
    assert(addr < map_size && addr + len <= map_size);
    assert(!(addr & 0x7) && !(len & 0x7));

    char* ptr = (char*)buf;

    while (len > 0) {
        size_t offset = addr & 0x7;
        unsigned int chunk = std::min(8 - offset, len);

        assert((chunk == 4) || (chunk == 8));

        if (chunk == 4) {
            *(uint32_t*)ptr = *(volatile uint32_t*)((char*)map_base + addr);
        } else {
            *(uint64_t*)ptr = *(volatile uint64_t*)((char*)map_base + addr);
        }

        len -= chunk;
        addr += chunk;
        ptr += chunk;
    }
}

void BARMemorySpace::write(Address addr, const void* buf, size_t len)
{
    spdlog::trace("BARMemorySpace::write({:#x}, {}, {})", addr, buf, len);

    addr -= iova_base;
    assert(addr < map_size && addr + len <= map_size);
    assert(!(addr & 0x7) && !(len & 0x7));

    char* ptr = (char*)buf;

    while (len > 0) {
        size_t offset = addr & 0x7;
        unsigned int chunk = std::min(8 - offset, len);

        assert((chunk == 4) || (chunk == 8));

        if (chunk == 4) {
            *(volatile uint32_t*)((char*)map_base + addr) = *(uint32_t*)ptr;
        } else {
            *(volatile uint64_t*)((char*)map_base + addr) = *(uint64_t*)ptr;
        }

        len -= chunk;
        addr += chunk;
        ptr += chunk;
    }
}

void BARMemorySpace::memset(Address addr, int c, size_t len)
{
    spdlog::trace("BARMemorySpace::memset({:#x}, {}, {})", addr, c, len);

    addr -= iova_base;
    assert(addr < map_size && addr + len <= map_size);
    assert(!(addr & 0x7) && !(len & 0x7));

    uint64_t val = c;
    val |= val << 8;
    val |= val << 16;
    val |= val << 32;

    while (len > 0) {
        size_t offset = addr & 0x7;
        unsigned int chunk = std::min(8 - offset, len);

        assert((chunk == 4) || (chunk == 8));

        if (chunk == 4) {
            *(volatile uint32_t*)((char*)map_base + addr) = (uint32_t)val;
        } else {
            *(volatile uint64_t*)((char*)map_base + addr) = val;
        }

        len -= chunk;
        addr += chunk;
    }
}
