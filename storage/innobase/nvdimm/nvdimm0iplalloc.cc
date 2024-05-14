
#ifdef UNIV_NVDIMM_IPL
#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>


void make_ppl_and_push_queue(buf_pool_t * buf_pool){
	uint start_index = nvdimm_info->static_ipl_page_number_per_buf_pool * buf_pool->instance_no;
	uint end_index = start_index + nvdimm_info->static_ipl_page_number_per_buf_pool;
	buf_pool->static_ipl_allocator = new boost::lockfree::queue<uint>(nvdimm_info->static_ipl_page_number_per_buf_pool);
	if (!buf_pool->static_ipl_allocator->is_lock_free())
		fprintf(stderr, "Not Lock Free\n");
//   mutex_enter(&buf_pool->static_allocator_mutex);
	for (uint i = start_index + 1; i <= end_index; i++){
		if(!buf_pool->static_ipl_allocator->push(i))
			fprintf(stderr, "Error : static_ipl_queue is full\n");
	}
//   mutex_exit(&buf_pool->static_allocator_mutex);
//   fprintf(stderr, "buf_pool %lu static allocator_size: %d allocator_start: %u, allocator_end: %u\n", buf_pool->instance_no, buf_pool->static_ipl_allocator->size(), buf_pool->static_ipl_allocator->back(), buf_pool->static_ipl_allocator->front());
}

unsigned char * alloc_ppl_from_queue(buf_pool_t * buf_pool){
	uint ipl_index = 0;
//   mutex_enter(&buf_pool->static_allocator_mutex);
	if(buf_pool->static_ipl_allocator->empty()){
		// fprintf(stderr, "Error : static_ipl_queue is empty\n");
		//   mutex_exit(&buf_pool->static_allocator_mutex);
		return NULL;
	}
	// fprintf(stderr, "Static,%u\n", buf_pool->static_ipl_allocator->front());

	buf_pool->static_ipl_allocator->pop(ipl_index);
	unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, ipl_index, nvdimm_info->each_ppl_size);
	//   fprintf(stderr, "Static,%f,%lu,%u\n", (double)(time(NULL) - my_start),buf_pool->instance_no, (nvdimm_info->static_ipl_page_number_per_buf_pool - buf_pool->static_ipl_allocator->size()) * 100 / nvdimm_info->static_ipl_page_number_per_buf_pool);
	//   mutex_exit(&buf_pool->static_allocator_mutex);

	return ret_address;
}

void free_ppl_and_push_queue(buf_pool_t * buf_pool, unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_static_address_to_indirection_queue Error: addr is NULL\n");
      return;
  }
  memset_to_cxl(addr, 0x00, nvdimm_info->each_ppl_size);
//   mutex_enter(&buf_pool->static_allocator_mutex);
  buf_pool->static_ipl_allocator->push(get_ipl_index_from_addr(nvdimm_info->static_start_pointer, addr, nvdimm_info->each_ppl_size));
//   mutex_exit(&buf_pool->static_allocator_mutex);
  // fprintf(stderr, "free static address : %p\n", addr);
}

//ipl index를 통해 해당 ipl의 주소를 찾아준다.
unsigned char * get_addr_from_ipl_index(unsigned char * start_ptr, uint index, uint64_t ipl_per_page_size){
  if(index == 0)  return NULL; // 할당되지 않은 경우
  unsigned char * ret_addr = start_ptr + ((uint64_t)(index - 1) * ipl_per_page_size);
  return ret_addr;
}

uint get_ipl_index_from_addr(unsigned char * start_ptr, unsigned char * ret_addr, uint64_t ipl_per_page_size){
  uint64_t ret_index = (uint64_t)(ret_addr - start_ptr) / ipl_per_page_size;
  return uint(ret_index + 1);
}

#endif