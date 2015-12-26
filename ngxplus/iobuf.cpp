
#include <string_printf.h>
#include "iobuf.h"
#include "log.h"
#include "info_log_context.h"

namespace ngxplus {

IOBuf::IOBuf() : _pool(nullptr),
        _block_size(DEFAULT_BLOCK_SIZE),
        _blocks(0),
        _bytes(0)
{
}

IOBuf::IOBuf(size_t size) : _pool(nullptr),
        _block_size(size),
        _blocks(0),
        _bytes(0)
{
    if (size < (MIN_PAYLOAD_SIZE + 2 * sizeof(ngx_pool_t))) {
        LOG(NGX_LOG_LEVEL_WARN, "pool's blocksize [%ld] LT [%ld]",
                size, MIN_PAYLOAD_SIZE + 2 * sizeof(ngx_pool_t));
        _block_size = MIN_PAYLOAD_SIZE + 2 * sizeof(ngx_pool_t);
    }
}

IOBuf::~IOBuf()
{
    if (_pool) {
        ngx_destroy_pool(_pool);
    }
}

// -1 : error
// postive num : alloced size
int IOBuf::alloc(char** buf, size_t size, IOBufAllocType type)
{
    if (!_pool) {
        _pool = ngx_create_pool(_block_size, NULL);
        if (!_pool) {
            LOG(NGX_LOG_LEVEL_ALERT, "create pool [blocksize=%ld] failed", _block_size);
            return -1;
        }
        if (_pool->max < (MIN_PAYLOAD_SIZE)) {
            LOG(NGX_LOG_LEVEL_ALERT, "pool's blocksize [%ld] too small", _block_size);
            return -1;
        }
    }

    if (size == 0) {
        LOG(NGX_LOG_LEVEL_WARN, "iobuf alloc size = 0");
        return -1;
    }

    if (size > _pool->max) {
        LOG(NGX_LOG_LEVEL_ALERT, "iobuf alloc size > pool.max,"
                "big_pool is not supported");
        return -1;
    }

    long remain = _pool->current->d.end - _pool->current->d.last;
    if ((type == IOBUF_ALLOC_SIMILAR) && (remain > 0)) {
        size = std::min((long)size, remain);
    }
    //LOG(NGX_LOG_LEVEL_NOTICE, "alloc size %ld", size);
    *buf = (char*)ngx_palloc(_pool, size);
    if (!(*buf)) {
        return -1;
    }

    if (_blocks == 0) {
        _read_point = *buf;
        _start_points[_blocks++] = _read_point;
    }
    // ensure data to be stored in series
    if (_pool->current->d.next) {
        _pool->current = _pool->current->d.next;
        _start_points[_blocks++] = *buf;
        if (_blocks > MAX_BLOCKS_NUM) {
            LOG(NGX_LOG_LEVEL_WARN, "_blocks GT MAX_BLOCKS_NUM"
                    "try to increase _block_size [%ld]", _block_size);
            return -1;
        }
    }
    _bytes += size;
    return size;
}

int IOBuf::alloc(char** buf)
{
    return alloc(buf, MIN_PAYLOAD_SIZE, IOBUF_ALLOC_SIMILAR);
}

void IOBuf::reclaim(int count)
{
    // the count must GE current block's size
    long current_block_len = (char*)_pool->current->d.last - _start_points[_blocks - 1];
    if (current_block_len <= count) {
        LOG(NGX_LOG_LEVEL_ALERT, "reclaim count GE current_block_len");
        count = current_block_len - 1;
    }
    _pool->current->d.last -= count;
    _bytes -= count;
}

size_t IOBuf::get_byte_count()
{
    return _bytes;
}

void IOBuf::dump_payload(std::string* payload)
{
    ngx_pool_t* p = _pool;
    int i = 0;
    while(p) {
        common::string_appendn(payload, _start_points[i],
                (size_t)((char*)p->d.last - _start_points[i]));
        i++;
    }
    return;
}

void IOBuf::dump_info(std::string* info)
{
    ngx_pool_t* p = _pool;
    int i = 0;
    while (p)
    {
        common::string_appendf(info, "p = %p\n", p);  
        common::string_appendf(info, "  .d\n");  
        common::string_appendf(info, "    .last = %p\n", p->d.last);  
        common::string_appendf(info, "    .end = %p\n", p->d.end);  
        common::string_appendf(info, "    .next = %p\n", p->d.next);  
        common::string_appendf(info, "    .failed = %ld\n", p->d.failed);  
        if (i == 0) {
            common::string_appendf(info, "  .max = %zu\n", p->max);  
            common::string_appendf(info, "  .current = %p\n", p->current);  
            common::string_appendf(info, "  .chain = %p\n", p->chain);  
            common::string_appendf(info, "  .large = %p\n", p->large);  
            common::string_appendf(info, "  .cleanup = %p\n", p->cleanup);  
            common::string_appendf(info, "  .log = %p\n", p->log);  
        }
        common::string_appendf(info, "available p memory = %ld\n", p->d.end - p->d.last);  
        common::string_appendf(info, "start_point = %p\n\n", _start_points[i++]);
        p = p->d.next;
    }
    common::string_appendf(info, "bytes = %zu\n", _bytes);
}

}
