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

std::tr1::unordered_map<page_id_t, unsigned char *> ipl_map;

unsigned char* nvdimm_ptr = NULL;
int nvdimm_fd = -1;
nvdimm_system * nvdimm_info = NULL;
/* Create or initialize NVDIMM mapping reginos
	 If a memroy-maped already exists then trigger recovery process and initialize

@param[in] path		mmaped file directory
@param[in] pool_size 	mmaped file size
@return true if mmaped file creation / initilzation is failed
*/


bool make_static_and_dynamic_ipl_region(){ //여기서 static 크기 바꿔주면 STATIC_MAX_SIZE 바꿔줘야함.
  nvdimm_info = static_cast<nvdimm_system *>(ut_zalloc_nokey(sizeof(*nvdimm_info)));
  nvdimm_info->static_ipl_size = (1024UL + 824) * 1024UL * 1024UL; // static ipl size : 1,8GB
  nvdimm_info->dynamic_ipl_size = 200 * 1024 * 1024; // dynamic ipl size : 0.2GB

  nvdimm_info->static_ipl_per_page_size = 1024; // per page static size : 1KB
  nvdimm_info->dynamic_ipl_per_page_size = 1024 * 8; // per page dynamic size : 8KB

  nvdimm_info->static_ipl_count = 0; 
  nvdimm_info->dynamic_ipl_count = 0;

  nvdimm_info->static_ipl_max_page_count = nvdimm_info->static_ipl_size / nvdimm_info->static_ipl_per_page_size; // static ipl max page count : 4M
  nvdimm_info->dynamic_ipl_max_page_count = nvdimm_info->dynamic_ipl_size / nvdimm_info->dynamic_ipl_per_page_size; // dynamic ipl max page count : 1M

  nvdimm_info->static_start_pointer = nvdimm_ptr;
  nvdimm_info->dynamic_start_pointer = nvdimm_ptr + nvdimm_info->static_ipl_per_page_size * nvdimm_info->static_ipl_max_page_count ;
  fprintf(stderr, "static start pointer : %p, dynamic start pointer : %p\n", nvdimm_info->static_start_pointer, nvdimm_info->dynamic_start_pointer);
  make_static_indirection_queue(nvdimm_info->static_start_pointer, nvdimm_info->static_ipl_per_page_size, nvdimm_info->static_ipl_max_page_count);
  make_dynamic_indirection_queue(nvdimm_info->dynamic_start_pointer, nvdimm_info->dynamic_ipl_per_page_size, nvdimm_info->dynamic_ipl_max_page_count);

  mutex_create(LATCH_ID_STATIC_REGION, &nvdimm_info->static_region_mutex);
  mutex_create(LATCH_ID_DYNAMIC_REGION, &nvdimm_info->dynamic_region_mutex);
  return true;
}


//1차 목표, static하게 구현한거 새로운 구조로 바꿔보기.

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
	}

	// Force to set NVIMMM
  setenv("PMEM_IS_PMEM_FORCE", "1", 1);
  NVDIMM_INFO_PRINT("Current kernel does not recognize NVDIMM as the persistenct memory \n \
      We force to set the environment variable PMEM_IS_PMEM_FORCE \n \
      We call mync() instead of mfense()\n");

  //make static and dynamic ipl region
  if(make_static_and_dynamic_ipl_region()){
    NVDIMM_INFO_PRINT("make static and dynamic ipl region success!\n");
  }
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
