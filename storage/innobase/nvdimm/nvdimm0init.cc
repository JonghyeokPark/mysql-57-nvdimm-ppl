#ifdef UNIV_NVDIMM_PPL
#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

// (anonymous): ipl-undo
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
time_t my_start = 0;

//PPL Lack
bool flush_thread_started = false;
ulint flush_thread_started_threshold = 0;

/* Create or initialize NVDIMM mapping reginos
	 If a memroy-maped already exists then trigger recovery process and initialize

@param[in] path		mmaped file directory
@param[in] pool_size 	mmaped file size
@return true if mmaped file creation / initilzation is failed
*/


bool make_static_and_dynamic_ipl_region
	( ulint number_of_buf_pool, 
	ulonglong nvdimm_overall_ppl_size, 
	ulint nvdimm_each_ppl_size,
	ulint nvdimm_max_ppl_size){ //여기서 static 크기 바꿔주면 STATIC_MAX_SIZE 바꿔줘야함.

	nvdimm_info = static_cast<nvdimm_system *>(ut_zalloc_nokey(sizeof(*nvdimm_info)));
	nvdimm_info->overall_ppl_size = nvdimm_overall_ppl_size; // static ipl size : 1,8GB
	nvdimm_info->each_ppl_size = nvdimm_each_ppl_size; // per page static size : 1KB
	nvdimm_info->max_ppl_size = nvdimm_max_ppl_size - (PPL_BLOCK_HDR_SIZE + ((nvdimm_max_ppl_size / nvdimm_each_ppl_size) - 1) * NTH_PPL_BLOCK_HEADER_SIZE);
	nvdimm_info->ppl_block_number_per_buf_pool = (nvdimm_info->overall_ppl_size / nvdimm_info->each_ppl_size) / number_of_buf_pool; 
	nvdimm_info->ppl_start_pointer = nvdimm_ptr;
	nvdimm_info->nvdimm_dwb_pointer = nvdimm_ptr + nvdimm_overall_ppl_size + (2 * 1024 * 1024 * 1024UL);
	nvdimm_info->nc_redo_start_pointer = nvdimm_ptr + nvdimm_overall_ppl_size + (3 * 1024 * 1024 * 1024UL);

	//PPL Lack
	flush_thread_started = true;
	flush_thread_started_threshold = (nvdimm_info->ppl_block_number_per_buf_pool * 5) / 100; // 5%

	
	fprintf(stderr, "Overall PPL Size : %luM\n", nvdimm_info->overall_ppl_size / (1024 * 1024));
	fprintf(stderr, "Each PPL Size : %lu\n", nvdimm_info->each_ppl_size);
	fprintf(stderr, "Max PPL Size : %lu\n", nvdimm_info->max_ppl_size);

	fprintf(stderr, "nvdimm_ptr: %p\n", nvdimm_ptr);
	fprintf(stderr, "static_start_pointer: %p\n", nvdimm_info->ppl_start_pointer);
	fprintf(stderr, "nc_redo_start_pointer: %p\n", nvdimm_info->nc_redo_start_pointer);

	my_start = time(NULL);
	return true;
}

unsigned char* nvdimm_create_or_initialize(const char* path, const uint64_t pool_size) {
 
	// (anonymous): check mmaped file existence
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
  	// TODO(anonymous): recovery process!   
		if (srv_use_nvdimm_ppl_recovery) {\
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

    // TODO(anonymous): optimize
    nvdimm_recv_running = false;
    //recv_ipl_parse_log();
 
	}
	// Force to set NVIMMM
  setenv("PMEM_IS_PMEM_FORCE", "1", 1);
  NVDIMM_INFO_PRINT("Current kernel does not recognize NVDIMM as the persistenct memory \n \
      We force to set the environment variable PMEM_IS_PMEM_FORCE \n \
      We call mync() instead of mfense()\n");
  /*Make NVDIMM structure*/

   /* mvcc-ppl */
  init_prebuilt_page_cache(prebuilt_page_list);
  /* end */
  
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

#endif
