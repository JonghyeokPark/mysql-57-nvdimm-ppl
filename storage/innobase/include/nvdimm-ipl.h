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
#include "buf0buf.h"
#include <queue>
#include <time.h>

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

/*IPL allocation*/
void make_static_indirection_queue(buf_pool_t * buf_pool);
unsigned char * alloc_static_address_from_indirection_queue(buf_pool_t * buf_pool);
void free_static_address_to_indirection_queue(buf_pool_t * buf_pool, unsigned char * addr);

void make_dynamic_indirection_queue(buf_pool_t * buf_pool);
unsigned char * alloc_dynamic_address_from_indirection_queue(buf_pool_t * buf_pool);
void free_dynamic_address_to_indirection_queue(buf_pool_t * buf_pool, unsigned char * addr);

void make_second_dynamic_indirection_queue(buf_pool_t * buf_pool);
unsigned char * alloc_second_dynamic_address_from_indirection_queue(buf_pool_t * buf_pool);
void free_second_dynamic_address_to_indirection_queue(buf_pool_t * buf_pool, unsigned char * addr);

unsigned char * get_addr_from_ipl_index(unsigned char * start_ptr, uint index, uint64_t ipl_per_page_size);
uint get_ipl_index_from_addr(unsigned char * start_ptr, unsigned char * ret_addr, uint64_t ipl_per_page_size);

/* IPL mapping */
bool make_static_and_dynamic_ipl_region(ulint number_of_buf_pool);
unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size);
void nvdimm_free(const uint64_t pool_size);



//Static reion info
#define PAGE_NO_OFFSET 4UL
#define DYNAMIC_ADDRESS_OFFSET 8UL
#define IPL_LOG_HEADER_SIZE 12UL
#define APPLY_LOG_HDR_SIZE 3UL

// Space | Page No | Dynamic_index || IPL Log | ...

// Dynamic region info
#define DIPL_HEADER_SIZE 4UL
// Second Dynamic index || IPL LOG | ..

extern time_t start;

enum ipl_flag {
  IPLIZED = 1,
  NORMALIZE = 2,
  DIRTIFIED = 4,
  IN_LOOK_UP = 8,
  SECOND_DIPL = 16
};

typedef struct NVDIMM_SYSTEM
{
  unsigned char* static_start_pointer;
  uint64_t static_ipl_size;
  uint64_t static_ipl_per_page_size;
  uint64_t static_ipl_page_number_per_buf_pool;
  
  unsigned char* dynamic_start_pointer;
  uint64_t dynamic_ipl_size;
  uint64_t dynamic_ipl_per_page_size;
  uint64_t dynamic_ipl_page_number_per_buf_pool;

  unsigned char* second_dynamic_start_pointer;
  uint64_t second_dynamic_ipl_size;
  uint64_t second_dynamic_ipl_per_page_size;
  uint64_t second_dynamic_ipl_page_number_per_buf_pool;

  unsigned char * nc_redo_start_pointer;
}nvdimm_system;

typedef struct APPLY_LOG_INFO
{
  byte * static_start_pointer;
  byte * dynamic_start_pointer;
  byte * second_dynamic_start_pointer;
  buf_block_t * block;

}apply_log_info;


// extern std::tr1::unordered_map<page_id_t, unsigned char *> ipl_map; // (page_id , ipl_static_address)
extern nvdimm_system * nvdimm_info;

/* IPL operations */
bool alloc_static_ipl_to_bpage(buf_page_t * bpage);
bool alloc_dynamic_ipl_to_bpage(buf_page_t * bpage);
bool alloc_second_dynamic_ipl_to_bpage(buf_page_t * bpage);
void nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, ulint rest_log_len);
bool can_write_in_ipl(buf_page_t * bpage, ulint log_len, ulint * rest_log_len);

//page ipl log apply 관련 함수들
void copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr);
void ipl_log_apply(byte * start_ptr, byte * end_ptr, apply_log_info * apply_info, mtr_t * temp_mtr);
void set_apply_info_and_log_apply(buf_block_t* block);


//page ipl info metadata 관련 함수들
void insert_page_ipl_info_in_hash_table(buf_page_t * bpage);
void nvdimm_ipl_add_split_merge_map(buf_page_t * bpage);
void normalize_ipl_page(buf_page_t * bpage, page_id_t page_id);
void set_for_ipl_page(buf_page_t* bpage);
bool check_not_flush_page(buf_page_t * bpage, buf_flush_t flush_type);
bool check_have_to_normalize_page_and_normalize(buf_page_t * bpage, buf_flush_t flush_type);
ulint get_can_write_size_from_write_pointer(buf_page_t * bpage, uint * type);
unsigned char * get_dynamic_ipl_pointer(buf_page_t * bpage);
unsigned char * get_second_dynamic_ipl_pointer(buf_page_t * bpage);
void set_flag(unsigned char * flags, ipl_flag flag);
void unset_flag(unsigned char * flags, ipl_flag flag);
bool get_flag(unsigned char * flags, ipl_flag flag);

//redo log buffer caching
struct nc_redo_buf{
  uint64_t nc_buf_free;
  uint64_t nc_lsn;
};

// redo info spot
#define REDO_INFO_OFFSET  (512*1024*1024)
extern nc_redo_buf* nc_redo_info;


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


#endif // end-of-header