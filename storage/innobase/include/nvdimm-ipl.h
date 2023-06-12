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
extern std::queue<unsigned char *> static_ipl_queue;
extern std::queue<unsigned char *> dynamic_ipl_queue;

/*IPL allocation*/
void make_static_indirection_queue(unsigned char * static_start_pointer, uint64_t static_ipl_size, uint static_ipl_max_page_count);
unsigned char * alloc_static_address_from_indirection_queue();
bool free_static_address_to_indirection_queue(unsigned char * addr);
void make_dynamic_indirection_queue(unsigned char * dynamic_start_pointer, uint64_t dynamic_ipl_size, uint dynamic_ipl_max_page_count);
unsigned char * alloc_dynamic_address_from_indirection_queue();
bool free_dynamic_address_to_indirection_queue(unsigned char * addr);

/* IPL mapping */
bool make_static_and_dynamic_ipl_region();
unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size);
void nvdimm_free(const uint64_t pool_size);




#define PAGE_NO_OFFSET 4UL
#define DYNAMIC_ADDRESS_OFFSET 8UL
#define IPL_LOG_HEADER_SIZE 16UL
#define STATIC_MAX_SIZE (1024UL - IPL_LOG_HEADER_SIZE)
typedef ib_mutex_t my_mutex;


struct IPL_INFO
{
  unsigned char* static_region_pointer;
  unsigned char* dynamic_region_pointer;
  ulint page_ipl_region_size;
  lsn_t oldest_modification;
  lsn_t newest_modification;
  bool is_dirtified;
};

typedef IPL_INFO ipl_info;

typedef struct NVDIMM_SYSTEM
{
  my_mutex ipl_map_mutex;
  my_mutex static_region_mutex;
  my_mutex dynamic_region_mutex;

  unsigned char* static_start_pointer;
  uint64_t static_ipl_size;
  uint static_ipl_per_page_size;
  uint static_ipl_count;
  uint static_ipl_max_page_count;
  
  unsigned char* dynamic_start_pointer;
  uint64_t dynamic_ipl_size;
  uint dynamic_ipl_per_page_size;
  uint dynamic_ipl_count;
  uint dynamic_ipl_max_page_count;

 
}nvdimm_system;

typedef struct APPLY_LOG_INFO
{
  byte * static_start_pointer;
  byte * dynamic_start_pointer;
  ulint log_len;
  ulint space_id;
  ulint page_no;
  buf_block_t * block;

}apply_log_info;


extern std::tr1::unordered_map<ulint,ipl_info *> ipl_map; // (page_id , offset in NVDIMM IPL regions)
extern nvdimm_system * nvdimm_info;

/* IPL operations */
unsigned char * get_static_ipl_address(page_id_t page_id);
bool alloc_dynamic_ipl_region(ipl_info * page_ipl_info);
ipl_info * alloc_static_ipl_info_and_region(page_id_t page_id);
ulint write_to_static_region(ipl_info * page_ipl_info, ulint len, unsigned char * write_ipl_log_buffer);
ulint write_to_dynamic_region(ipl_info * page_ipl_info, ulint len, unsigned char * write_ipl_log_buffer);
bool write_ipl_log_header_and_body(buf_page_t * bpage, ulint len, mlog_id_t type, unsigned char * log);
bool nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage);

//page ipl log apply 관련 함수들
bool copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr);
void ipl_log_apply(byte * start_ptr, apply_log_info * apply_info, mtr_t * temp_mtr);
void nvdimm_ipl_log_apply(buf_block_t* block);


//page ipl info metadata 관련 함수들
void insert_page_ipl_info_in_hash_table(buf_page_t * bpage);
void nvdimm_ipl_add_split_merge_map(page_id_t page_id);
bool nvdimm_ipl_remove_split_merge_map(buf_page_t * bpage, page_id_t page_id);
bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id);
void set_for_ipl_page(buf_page_t* bpage);
void print_page_info(buf_page_t * bpage);
bool check_not_flush_page(buf_page_t * bpage, buf_flush_t flush_type);
bool check_clean_checkpoint_page(buf_page_t * bpage);
void make_dirty_clean_checkpoint_page(buf_page_t * bpage);
bool is_dirtified_page(buf_page_t * bpage);



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

/*
unsigned char* nvdimm_ipl_log_apply(
	mlog_id_t type,
	unsigned char* ptr,
	unsigned char* end_ptr,
	unsigned long space,
	unsigned long page_no,
	buf_block_t* block);
*/

#endif // end-of-header