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
#include <queue>
#include <time.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include "buf0buf.h"
#include "dyn0buf.h"
extern struct timeval start, end;

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
extern time_t my_start;

/*IPL allocation*/
void make_ppl_and_push_queue(buf_pool_t * buf_pool);
unsigned char * alloc_ppl_from_queue(buf_pool_t * buf_pool);
void free_ppl_and_push_queue(buf_pool_t * buf_pool, unsigned char * addr);

unsigned char * get_addr_from_ppl_index(unsigned char * start_ptr, uint index, uint64_t ipl_per_page_size);
uint get_ppl_index_from_addr(unsigned char * start_ptr, unsigned char * ret_addr, uint64_t ipl_per_page_size);

/* IPL mapping */
bool make_static_and_dynamic_ipl_region
        ( ulint number_of_buf_pool, 
          ulonglong nvdimm_overall_ppl_size,
          ulint nvdimm_each_ppl_size,
		  ulint nvdimm_max_ppl_size);
unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size);
void nvdimm_free(const uint64_t pool_size);


/* space (4) | page_no (4) | First_Dynamic_index (4) | length (4) | LSN (8) | Normalize Flag(1) | mtr_log | mtr_log | ... */
/* IPL_LOG_HEADER OFFSET */
#define PPL_HDR_FIRST_MARKER					0
#define PPL_HDR_NORMALIZE_MARKER				1
#define IPL_HDR_SPACE							2
#define IPL_HDR_PAGE							6
#define PPL_HDR_DYNAMIC_INDEX					10
#define PPL_HDR_LEN								14
#define PPL_HDR_LSN								18	
#define PPL_BLOCK_HDR_SIZE							26

/* Second_Dynamic_index (4) | mtr_log | ... */
#define NTH_IPL_BLOCK_MARKER				0
#define NTH_PPL_DYNAMIC_INDEX			 	1
#define NTH_PPL_BLOCK_HEADER_SIZE 				5

/* In apply log strucrute*/
/* mtr_log_type(1) | mtr_body_len (2) | trx_id (8) | mtr_log_body(1 ~ 110) | */
#define APPLY_LOG_HDR_SIZE 11UL


enum ipl_flag {
  PPLIZED = 1,
  NORMALIZE = 2,
  DIRTIFIED = 4,
  IN_LOOK_UP = 8,
  DIRECTLY_WRITE = 16,
  IN_PPL_BUF_POOL = 32
};

typedef struct NVDIMM_SYSTEM
{
  unsigned char* ppl_start_pointer;
  uint64_t overall_ppl_size;
  uint64_t each_ppl_size;
  uint64_t ppl_block_number_per_buf_pool;
  uint64_t max_ppl_size;

  unsigned char* dynamic_start_pointer;
  uint64_t dynamic_ipl_size;
  uint64_t dynamic_ipl_per_page_size;
  uint64_t dynamic_ipl_page_number_per_buf_pool;

  unsigned char* second_dynamic_start_pointer;
  uint64_t second_dynamic_ipl_size;
  uint64_t second_dynamic_ipl_per_page_size;
  uint64_t second_dynamic_ipl_page_number_per_buf_pool;

  unsigned char * nc_redo_start_pointer;
  unsigned char * nvdimm_dwb_pointer;
}nvdimm_system;

typedef struct APPLY_LOG_INFO
{
  byte * static_start_pointer;
  byte * dynamic_start_pointer;
  byte * second_dynamic_start_pointer;
  uint ipl_log_length;
  buf_block_t * block;

}apply_log_info;

//PPL Lack
extern bool flush_thread_started;
extern ulint flush_thread_started_threshold;


bool check_write_hot_page(buf_page_t * bpage, lsn_t lsn);
//PPL Lack

// extern std::tr1::unordered_map<page_id_t, unsigned char *> ipl_map; // (page_id , ipl_static_address)
extern nvdimm_system * nvdimm_info;

/* IPL operations */

//Page별 IPL allocation function
bool alloc_first_ppl_to_bpage(buf_page_t * bpage);
bool alloc_nth_ppl_to_bpage(buf_page_t * bpage);
void copy_log_to_memory(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id);
void copy_log_to_ppl_directly(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id);
bool copy_memory_log_to_nvdimm(buf_page_t * bpage);
bool copy_memory_log_to_ppl(unsigned char *log, ulint len, buf_page_t * bpage);

//page ipl log apply 관련 함수들
void set_apply_info_and_log_apply(buf_block_t* block);
void all_ppl_apply_to_page(byte *start_ptr, ulint apply_log_size, buf_block_t *block, mtr_t *temp_mtr);
byte* fetch_next_segment(byte* current_end, byte** new_end, byte** next_ppl);
void apply_log_record(mlog_id_t log_type, byte* log_data, uint length, trx_id_t trx_id, buf_block_t* block, mtr_t* temp_mtr);
// byte * ipl_complete_log_apply(byte * apply_ptr, uint * ppl_left_size, buf_block_t * block, mtr_t * temp_mtr);



//page Normalize, lookup table 함수들
void insert_page_ppl_info_in_hash_table(buf_page_t * bpage);
void set_normalize_flag(buf_page_t * bpage, uint normalize_cause);
void normalize_ppled_page(buf_page_t * bpage, page_id_t page_id);
void set_for_ppled_page(buf_page_t* bpage);
bool check_can_be_pplized(buf_page_t * bpage);
bool check_can_be_skip(buf_page_t *bpage);
bool check_return_ppl_region(buf_page_t * bpage);
unsigned char * get_last_block_address_index(buf_page_t * bpage);


//ipl metadata set, get 함수들
void set_ppl_length_in_ppl_header(buf_page_t * bpage, ulint length);
uint get_ppl_length_from_ppl_header(buf_page_t * bpage);
void set_page_lsn_in_ppl_header(unsigned char* first_ppl_block_ptr, lsn_t lsn);
lsn_t get_page_lsn_from_ppl_header(unsigned char* first_ppl_block_ptr);
void set_normalize_flag_in_ppl_header(unsigned char * first_ppl_block_ptr);
unsigned char get_normalize_flag_in_ppl_header(unsigned char * first_ppl_block_ptr);

//page IPL flag 관련 함수
void set_flag(unsigned char * flags, ipl_flag flag);
void unset_flag(unsigned char * flags, ipl_flag flag);
bool get_flag(unsigned char * flags, ipl_flag flag);

//Copy to NVDIMM and flush cache 
void memcpy_to_nvdimm(void *dest, void *src, size_t size);
// void memcpy_to_nvdimm(void *__restrict dst, const void * __restrict src, size_t n);
void memset_to_nvdimm(void* dest, int value, size_t size);

//PPL시킬 수 있는지 판별하는 함수
bool can_page_be_pplized(const byte* ptr, const byte* end_ptr);

struct mem_to_nvdimm_copy_t{

	buf_page_t * bpage;

	void init(buf_page_t * target_page){ // 추후 calloc 체킹 필요
		bpage = target_page;
	}

	bool operator()(const ppl_mtr_buf_t::block_t* block)
	{
		return copy_memory_log_to_ppl((unsigned char *)(block->begin()), block->used(), bpage);
	}
};


/* PPL NVDIMM BUFFER FUNCTION*/
ulint
ppl_buf_page_read_in_area(
	std::vector <page_id_t>	page_id_list, /*Page_id List*/
	uint read_page_no,
	buf_pool_t * buf_pool);

void
ppl_buf_flush_note_modification(
/*=============================*/
	buf_block_t*	block);	/*!< in: end lsn of the last mtr in the
					set of mtr's */

/* PPL NVDIMM BUFFER FUNCTION*/


//redo log buffer caching
struct nc_redo_buf{
  uint64_t nc_buf_free;
  uint64_t nc_lsn;
};
#define REDO_INFO_OFFSET  (512*1024*1024)
extern nc_redo_buf* nc_redo_info;


#ifdef UNIV_NVDIMM_PPL
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
	SIPL,
	DIPL,
	SDIPL
} RECV_IPL_PAGE_TYPE;

void recv_ipl_parse_log();
void recv_ipl_map_print();
void recv_ipl_apply(buf_block_t* block);
void recv_ipl_set_len(unsigned char* ipl_ptr, uint32_t diff);
uint32_t recv_ipl_get_len(unsigned char* ipl_ptr);
void recv_ipl_set_lsn(unsigned char* ipl_ptr, lsn_t lsn);
lsn_t recv_ipl_get_lsn(unsigned char* ipl_ptr);
void recv_ipl_set_wp(unsigned char* ipl_ptr, uint32_t cur_len);
ulint recv_ipl_get_wp(unsigned char* ipl_ptr);

bool recv_copy_log_to_mem_to_apply(apply_log_info * apply_info
																	, mtr_t * temp_mtr
																	, ulint real_size
																	, lsn_t page_lsn);

void recv_ipl_log_apply(byte * start_ptr
											, byte * end_ptr
											, apply_log_info * apply_info
											, mtr_t * temp_mtr);

void recv_clean_ipl_map();
lsn_t recv_get_first_ipl_lsn(buf_block_t* block);
bool recv_check_normal_flag(buf_block_t* block);
lsn_t recv_get_first_ipl_lsn_using_page_id(page_id_t page_id);
bool recv_check_normal_flag_using_page_id(page_id_t page_id);

RECV_IPL_PAGE_TYPE recv_check_iplized(page_id_t page_id);
extern std::tr1::unordered_map<page_id_t, uint64_t > ipl_recv_map;

// (jhpark): for IPL-undo
extern std::tr1::unordered_map<uint64_t, uint64_t > ipl_active_trx_ids;
extern bool nvdimm_recv_ipl_undo;
extern uint64_t ipl_skip_apply_cnt;
extern uint64_t ipl_org_apply_cnt;

#endif // end-of-header
