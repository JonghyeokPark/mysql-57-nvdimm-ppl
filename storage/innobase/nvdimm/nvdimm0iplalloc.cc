
#ifdef UNIV_NVDIMM_IPL
#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

// bool is_flushed_thread = false;


void make_ppl_and_push_queue(buf_pool_t * buf_pool){
	uint start_index = nvdimm_info->static_ipl_page_number_per_buf_pool * buf_pool->instance_no;
	uint end_index = start_index + nvdimm_info->static_ipl_page_number_per_buf_pool;
	buf_pool->static_ipl_allocator = new std::queue<uint>;
	mutex_enter(&buf_pool->static_allocator_mutex);
	for (uint i = start_index + 1; i <= end_index; i++){
		buf_pool->static_ipl_allocator->push(i);
	}
	mutex_exit(&buf_pool->static_allocator_mutex);
  fprintf(stderr, "buf_pool %lu static allocator_size: %d allocator_start: %u, allocator_end: %u\n", buf_pool->instance_no, buf_pool->static_ipl_allocator->size(), buf_pool->static_ipl_allocator->back(), buf_pool->static_ipl_allocator->front());
}

unsigned char * alloc_ppl_from_queue(buf_pool_t * buf_pool){
	mutex_enter(&buf_pool->static_allocator_mutex);
	if(buf_pool->static_ipl_allocator->empty()){
		// fprintf(stderr, "Error : static_ipl_queue is empty\n");
		mutex_exit(&buf_pool->static_allocator_mutex);
		return NULL;
	}
	// fprintf(stderr, "Static,%u\n", buf_pool->static_ipl_allocator->front());
	unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, buf_pool->static_ipl_allocator->front(), nvdimm_info->each_ppl_size);
	buf_pool->static_ipl_allocator->pop();
	// fprintf(stderr, "Static,%f,%lu,%u\n", (double)(time(NULL) - my_start),buf_pool->instance_no, (nvdimm_info->static_ipl_page_number_per_buf_pool - buf_pool->static_ipl_allocator->size()) * 100 / nvdimm_info->static_ipl_page_number_per_buf_pool);
	mutex_exit(&buf_pool->static_allocator_mutex);

	// PPL Cleaner Buffer Pool Flush
	// if(!is_flushed_thread && ((nvdimm_info->static_ipl_page_number_per_buf_pool - buf_pool->static_ipl_allocator->size()) * 100 / nvdimm_info->static_ipl_page_number_per_buf_pool) >= 90){
	// 	os_event_set(ppl_buf_flush_event);
	// 	is_flushed_thread = true;
	// }

	// if(!is_ppl_lack && buf_pool->static_ipl_allocator->size() < ppl_lack_threshold){
	// 	is_ppl_lack = true;
	// }
	
	return ret_address;
}

// 두개 할당되어 있으면 두개 return 되는지 확인 필요함
void free_ppl_and_push_queue(buf_pool_t * buf_pool, unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_static_address_to_indirection_queue Error: addr is NULL\n");
      return;
  }
	unsigned char * next_addr = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, mach_read_from_4(addr + IPL_HDR_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
	memset_to_cxl(addr, 0, nvdimm_info->each_ppl_size);
	mutex_enter(&buf_pool->static_allocator_mutex);
	buf_pool->static_ipl_allocator->push(get_ipl_index_from_addr(nvdimm_info->static_start_pointer, addr, nvdimm_info->each_ppl_size));
	mutex_exit(&buf_pool->static_allocator_mutex);
	// fprintf(stderr, "free static address : %p\n", addr);
	// if((uint64_t)addr % 128 != 0){
	// 	fprintf(stderr, "Error : addr is not aligned: %p\n", addr);
	// }
	addr = next_addr;
	while(addr != NULL){
		next_addr = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, mach_read_from_4(addr), nvdimm_info->each_ppl_size);
		memset_to_cxl(addr, 0, nvdimm_info->each_ppl_size);
		// fprintf(stderr, "free Nth static address : %p\n", addr);
		// if((uint64_t)addr % 128 != 0){
		// 	fprintf(stderr, "Error : Nth addr is not aligned: %p\n", addr);
		// }
		mutex_enter(&buf_pool->static_allocator_mutex);
		buf_pool->static_ipl_allocator->push(get_ipl_index_from_addr(nvdimm_info->static_start_pointer, addr, nvdimm_info->each_ppl_size));
		mutex_exit(&buf_pool->static_allocator_mutex);
		addr = next_addr;
	}
	
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
