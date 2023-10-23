// Copyright 2022 VLDB Lab. (http://vldb.skku.ac.kr/)
// Author: Jonghyeok Park
// E-mail: akindo19@skku.edu

#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

// (jhpark): ipl-undo
bool nvdimm_recv_ipl_undo = false;
std::tr1::unordered_map<uint64_t, uint64_t > ipl_active_trx_ids;
uint64_t ipl_skip_apply_cnt = 0;
uint64_t ipl_org_apply_cnt = 0;

std::tr1::unordered_map<page_id_t, unsigned char *> ipl_map;

unsigned char* nvdimm_ptr = NULL;
int nvdimm_fd = -1;
nvdimm_system * nvdimm_info = NULL;
nc_redo_buf * nc_redo_info = NULL;

/* recovery */
bool nvdimm_recv_running = false;
unsigned char* nvdimm_recv_ptr = NULL;

/* Create or initialize NVDIMM mapping reginos
	 If a memroy-maped already exists then trigger recovery process and initialize

@param[in] path		mmaped file directory
@param[in] pool_size 	mmaped file size
@return true if mmaped file creation / initilzation is failed
*/


bool make_static_and_dynamic_ipl_region(ulint number_of_buf_pool){ //여기서 static 크기 바꿔주면 STATIC_MAX_SIZE 바꿔줘야함.
  nvdimm_info = static_cast<nvdimm_system *>(ut_zalloc_nokey(sizeof(*nvdimm_info)));
  nvdimm_info->static_ipl_size = (1024 + 824) * 1024UL * 1024UL; // static ipl size : 1,8GB
  nvdimm_info->dynamic_ipl_size = (100) * 1024 * 1024; // dynamic ipl size : 0.2GB
  nvdimm_info->second_dynamic_ipl_size = (100) * 1024 * 1024; // dynamic ipl size : 0.2GB

  nvdimm_info->static_ipl_per_page_size = 256; // per page static size : 1KB
  nvdimm_info->dynamic_ipl_per_page_size = 256; // per page dynamic size : 8KB
  nvdimm_info->second_dynamic_ipl_per_page_size = 1024 * 2;

  nvdimm_info->static_ipl_page_number_per_buf_pool = (nvdimm_info->static_ipl_size / nvdimm_info->static_ipl_per_page_size) / number_of_buf_pool; // 
  nvdimm_info->dynamic_ipl_page_number_per_buf_pool = (nvdimm_info->dynamic_ipl_size / nvdimm_info->dynamic_ipl_per_page_size) / number_of_buf_pool; // dynamic ipl max page count : 1M
  nvdimm_info->second_dynamic_ipl_page_number_per_buf_pool = (nvdimm_info->second_dynamic_ipl_size / nvdimm_info->second_dynamic_ipl_per_page_size) / number_of_buf_pool; // dynamic ipl max page count : 1M

  nvdimm_info->static_start_pointer = nvdimm_ptr;
  nvdimm_info->dynamic_start_pointer = nvdimm_ptr + nvdimm_info->static_ipl_size;
  nvdimm_info->second_dynamic_start_pointer = nvdimm_info->dynamic_start_pointer + nvdimm_info->dynamic_ipl_size;
  nvdimm_info->nc_redo_start_pointer = nvdimm_info->second_dynamic_start_pointer + (2 * 1024 * 1024 * 1024UL);
/*
  fprintf(stderr, "static start pointer : %p, dynamic start pointer : %p, second dynamic start pointer: %p, nc_redo: %p\n", nvdimm_info->static_start_pointer, nvdimm_info->dynamic_start_pointer, nvdimm_info->second_dynamic_start_pointer, nvdimm_info->nc_redo_start_pointer);
  fprintf(stderr, "static IPL size per buf_pool : %u\n", nvdimm_info->static_ipl_page_number_per_buf_pool);
  fprintf(stderr, "Dynamic IPL size per buf_pool : %u\n", nvdimm_info->dynamic_ipl_page_number_per_buf_pool);
  fprintf(stderr, "Second Dynamic IPL size per buf_pool : %u\n", nvdimm_info->second_dynamic_ipl_page_number_per_buf_pool);
  //start = time(NULL);
*/
  return true;
}

unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size) {
 
	// (jhpark): check mmaped file existence
	if (access(path, F_OK) != 0) {
    nvdimm_fd = open(path, O_RDWR|O_CREAT, 0777);
    if (nvdimm_fd < 0) {
			NVDIMM_ERROR_PRINT("NVDIMM mmaped file open failed!\n");
      return NULL;
    }

    if (truncate(path, pool_size) == -1) {
			NVDIMM_ERROR_PRINT("NVDIMM mmaped file truncate failed!\n");
      return NULL;
    }

    nvdimm_ptr = (unsigned char *) mmap(NULL, pool_size, PROT_READ|PROT_WRITE, MAP_SHARED, nvdimm_fd, 0);
		memset(nvdimm_ptr, 0x00, pool_size);
		NVDIMM_INFO_PRINT("NVDIMM mmaped success!\n");

  } else {
  	// TODO(jhpark): recovery process!   
		if (srv_use_nvdimm_ipl_recovery) {\
			ib::info() << "We use IPLzied recovery mode!";
		}
		
		nvdimm_fd = open(path, O_RDWR, 0777);

  if (nvdimm_fd < 0) {
      NVDIMM_ERROR_PRINT("NVDIMM mmaped file open failed!\n");
      return NULL;
    }

    nvdimm_ptr = (unsigned char *) mmap(NULL, pool_size, PROT_READ|PROT_WRITE, MAP_SHARED, nvdimm_fd, 0);
    if (nvdimm_ptr == MAP_FAILED) {
      NVDIMM_ERROR_PRINT("NVDIMM mmap is failed!\n");
      return NULL;
    }

    // TODO(jhpark): optimize
    nvdimm_recv_running = true;
    //recv_ipl_parse_log();
 
	}
	// Force to set NVIMMM
  setenv("PMEM_IS_PMEM_FORCE", "1", 1);
  NVDIMM_INFO_PRINT("Current kernel does not recognize NVDIMM as the persistenct memory \n \
      We force to set the environment variable PMEM_IS_PMEM_FORCE \n \
      We call mync() instead of mfense()\n");
  /*Make NVDIMM structure*/
  
  return nvdimm_ptr;
}

/* Unmmaped mmaped file

@param[in] pool_size mmaped m]file size
*/
void nvdimm_free(const uint64_t pool_size) {
  munmap(nvdimm_ptr, pool_size);
  close(nvdimm_fd);
  NVDIMM_INFO_PRINT("munmap nvdimm mmaped file\n");
}





//고민을 해보면, 각 page별 ipl 영역에 space_id, page_no를 지정하면 굳이 hashtable을 저장할 필요가 없다.
//hash table은 메모리에 존재해도 됨.
