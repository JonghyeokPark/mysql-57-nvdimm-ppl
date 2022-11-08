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


unsigned char* nvdimm_ptr = NULL;
int nvdimm_fd = -1;

/* Create or initialize NVDIMM mapping reginos
	 If a memroy-maped already exists then trigger recovery process and initialize

@param[in] path		mmaped file directory
@param[in] pool_size 	mmaped file size
@return true if mmaped file creation / initilzation is failed
*/
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

	// (jhpark): actually, nvdimm_ptr is extern variable ... 
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
