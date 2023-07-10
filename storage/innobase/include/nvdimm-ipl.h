// Copyright 2022 VLDB Lab. (http://vldb.skku.ac.kr/)
// Author: Jonghyeok Park
// E-mail: akindo19@skku.edu

#ifndef __NVDIMM_IPL_H_
#define __NVDIMM_IPL_H_

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <tr1/unordered_map>
#include "buf0buf.h"
#include <queue>
#include <vector>

// TDOO(jhpark): make this variable configurable

#define NVDIMM_MMAP_FILE_NAME         			"nvdimm_mmap_file"
#define NVDIMM_MMAP_MAX_FILE_NAME_LENGTH    10000

#define NVDIMM_INFO_PRINT(fmt, args...)              \
  fprintf(stderr, "[NVDIMM_INFO]: %s:%d:%s():" fmt,  \
  __FILE__, __LINE__, __func__, ##args)                \

#define NVDIMM_ERROR_PRINT(fmt, args...)             \
  fprintf(stderr, "[NVDIMM_ERROR]: %s:%d:%s():" fmt, \
  __FILE__, __LINE__, __func__, ##args)                \


// (jhpark): persistence barrier
#define CACHE_LINE_SIZE 64
#define ASMFLUSH(dst) __asm__ __volatile__ ("clflush %0" : : "m"(*(volatile char *)dst))

static inline void clflush(volatile char* __p) {
  asm volatile("clflush %0" : "+m" (*__p));
}

static inline void mfence() {
  asm volatile("mfence":::"memory");
  return;
}


static inline void flush_cache(void *ptr, size_t size){
  unsigned int  i=0;
  uint64_t addr = (uint64_t)ptr;

  mfence();
  for (i =0; i < size; i=i+CACHE_LINE_SIZE) {
    clflush((volatile char*)addr);
    addr += CACHE_LINE_SIZE;
  }
  mfence();
}
static inline void memcpy_persist
                    (void *dest, void *src, size_t size){
  unsigned int  i=0;
  uint64_t addr = (uint64_t)dest;

  memcpy(dest, src, size);

  mfence();
  for (i =0; i < size; i=i+CACHE_LINE_SIZE) {
    clflush((volatile char*)addr);
    addr += CACHE_LINE_SIZE;
  }
  mfence();
}

extern unsigned char* nvdimm_ptr;
extern int nvdimm_fd;
extern std::queue<uint> static_ipl_queue;
extern std::queue<uint> dynamic_ipl_queue;

/*IPL allocation*/
void make_static_indirection_queue(unsigned char * static_start_pointer, uint64_t static_ipl_size, uint static_ipl_max_page_count);
unsigned char * alloc_static_address_from_indirection_queue();
bool free_static_address_to_indirection_queue(unsigned char * addr);
void make_dynamic_indirection_queue(unsigned char * dynamic_start_pointer, uint64_t dynamic_ipl_size, uint dynamic_ipl_max_page_count);
unsigned char * alloc_dynamic_address_from_indirection_queue();
bool free_dynamic_address_to_indirection_queue(unsigned char * addr);
unsigned char * get_addr_from_ipl_index(unsigned char * start_ptr, uint index, uint64_t ipl_per_page_size);
uint get_ipl_index_from_addr(unsigned char * start_ptr, unsigned char * ret_addr, uint64_t ipl_per_page_size);

/* IPL mapping */
bool make_static_and_dynamic_ipl_region();
unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size);
void nvdimm_free(const uint64_t pool_size);


#define PAGE_NO_OFFSET 4UL
#define DYNAMIC_ADDRESS_OFFSET 8UL
// TODO(jhpark): for recovery test; original: 12UL
#define IPL_LOG_HEADER_SIZE 18UL
#define APPLY_LOG_HDR_SIZE 3UL

enum ipl_flag {
  IPLIZED = 1,
  NORMALIZE = 2,
  DIRTIFIED = 4
};


typedef ib_mutex_t my_mutex;

typedef struct NVDIMM_SYSTEM
{
  // my_mutex ipl_map_mutex;
  my_mutex static_region_mutex;
  my_mutex dynamic_region_mutex;
  rw_lock_t lookup_table_lock;

  unsigned char* static_start_pointer;
  uint64_t static_ipl_size;
  uint64_t static_ipl_per_page_size;
  uint static_ipl_count;
  uint64_t static_ipl_max_page_count;
  
  unsigned char* dynamic_start_pointer;
  uint64_t dynamic_ipl_size;
  uint64_t dynamic_ipl_per_page_size;
  uint dynamic_ipl_count;
  uint64_t dynamic_ipl_max_page_count;
}nvdimm_system;

typedef struct APPLY_LOG_INFO
{
  byte * static_start_pointer;
  byte * dynamic_start_pointer;
  ulint space_id;
  ulint page_no;
  buf_block_t * block;

}apply_log_info;

namespace std {
    namespace tr1 {
        template <>
        struct hash<page_id_t> {
            size_t operator()(const page_id_t& key) const {
                // space와 page_no를 해싱하여 해시 값을 반환합니다.
                // 해싱 로직을 구현합니다.
                size_t spaceHash = std::tr1::hash<ib_uint32_t>()(key.space());
                size_t pageHash = std::tr1::hash<ib_uint32_t>()(key.page_no());

                return spaceHash ^ pageHash;
            }
        };
    }
}

extern std::tr1::unordered_map<page_id_t, unsigned char *> ipl_map; // (page_id , ipl_static_address)
extern nvdimm_system * nvdimm_info;

/* IPL operations */
void alloc_static_ipl_to_bpage(buf_page_t * bpage);
bool alloc_dynamic_ipl_region(buf_page_t * bpage);
ulint write_to_static_region(buf_page_t * bpage, ulint len, unsigned char * write_ipl_log_buffer);
ulint write_to_dynamic_region(buf_page_t * bpage, ulint len, unsigned char * write_ipl_log_buffer);
bool write_ipl_log_header_and_body(buf_page_t * bpage, ulint len, mlog_id_t type, unsigned char * log);
bool nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage);

//page ipl log apply 관련 함수들
bool copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr);
void ipl_log_apply(byte * start_ptr, apply_log_info * apply_info, mtr_t * temp_mtr);
void set_apply_info_and_log_apply(buf_block_t* block);


//page ipl info metadata 관련 함수들
void insert_page_ipl_info_in_hash_table(buf_page_t * bpage);
void nvdimm_ipl_add_split_merge_map(page_id_t page_id);
bool normalize_ipl_page(buf_page_t * bpage, page_id_t page_id);
bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id);
void set_for_ipl_page(buf_page_t* bpage);
bool check_not_flush_page(buf_page_t * bpage, buf_flush_t flush_type);
bool check_clean_checkpoint_page(buf_page_t * bpage, bool is_single_page_flush);
void check_have_to_normalize_page_and_normalize(buf_page_t * bpage, buf_flush_t flush_type);
ulint get_ipl_length_from_write_pointer(buf_page_t * bpage);
unsigned char * get_dynamic_ipl_pointer(buf_page_t * bpage);
void set_flag(unsigned char * flags, ipl_flag flag);
void unset_flag(unsigned char * flags, ipl_flag flag);
bool get_flag(unsigned char * flags, ipl_flag flag);


#ifdef UNIV_NVDIMM_IPL
unsigned char* 
recv_parse_or_apply_log_rec_body(
	mlog_id_t type,
	unsigned char* ptr,
	unsigned char* end_ptr,
	unsigned long space,
	unsigned long page_no,
	buf_block_t* block,
	mtr_t* mtr);
#endif



/* recovery */
extern unsigned char* nvdimm_recv_ptr;
extern bool nvdimm_recv_running;
typedef enum {
	NORMAL = 0,
	STATIC = 1,
	DYNAMIC = 2
} RECV_IPL_PAGE_TYPE;

void recv_ipl_parse_log();
void recv_ipl_map_print();
void recv_ipl_apply(buf_block_t* block);
void recv_ipl_set_flush_bit(unsigned char* ipl_ptr);
ulint recv_ipl_get_flush_bit(unsigned char* ipl_ptr);

RECV_IPL_PAGE_TYPE recv_check_iplized(page_id_t page_id);
extern std::tr1::unordered_map<page_id_t, std::vector<uint64_t> > ipl_recv_map;

#endif // end-of-header
