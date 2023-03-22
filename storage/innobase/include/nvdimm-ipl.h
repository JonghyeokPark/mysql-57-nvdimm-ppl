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


// TDOO(jhpark): make this variable configurable
#define NVDIMM_MAP_SIZE	ULONG_MAX;

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

/* IPL mapping */
unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size);
void nvdimm_free(const uint64_t pool_size);


// compare for page_id
struct comp
{
    bool operator()(const page_id_t &l, const page_id_t &r) const
    {
        if (l.space() == r.space()) {
            return l.page_no() > r.page_no();
        }
 
        return l.space() < r.space();
    }
};
typedef ib_mutex_t my_mutex;


typedef struct IPL_INFO
{
  uint64_t ipl_start_offset;
  ulint ipl_write_pointer;
  bool have_to_flush;
  my_mutex ipl_per_page_mutex;
}ipl_info;

typedef struct NVDIMM_SYSTEM
{
  uint64_t nvdimm_offset;
  my_mutex nvdimm_offset_mutex;
  my_mutex ipl_map_mutex;
}nvdimm_system;


extern std::tr1::unordered_map<ulint,ipl_info *> ipl_map; // (page_id , offset in NVDIMM IPL regions)
extern nvdimm_system * nvdimm_info;

// global offset which manages overall NVDIMM region
#define IPL_LOG_REGION_SZ	(1024UL*8UL) // 128KB로 변경, 많은 page가 생성.


// log header
typedef struct ipl_log_header {
  ulint body_len; //log를 적용할 len
  mlog_id_t type; // log의 type 저장.
} IPL_LOG_HDR;

/* IPL operations */
void alloc_new_ipl_info(page_id_t page_id);
bool nvdimm_ipl_add(const page_id_t page_id, unsigned char *log, ulint len, mlog_id_t type);
void nvdimm_ipl_log_apply(page_id_t page_id, buf_block_t* block);
//bool nvdimm_ipl_merge(page_id_t page_id, buf_page_t * page);

bool nvdimm_ipl_lookup(page_id_t page_id);
void nvdimm_ipl_add_split_merge_map(page_id_t page_id);
bool nvdimm_ipl_remove_split_merge_map(page_id_t page_id);
bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id);



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

