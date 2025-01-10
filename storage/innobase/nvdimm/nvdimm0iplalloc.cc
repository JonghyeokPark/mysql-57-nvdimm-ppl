
#ifdef UNIV_NVDIMM_PPL
#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>


void make_ppl_and_push_queue(buf_pool_t * buf_pool){
	uint start_index = nvdimm_info->ppl_block_number_per_buf_pool * buf_pool->instance_no;
	uint end_index = start_index + nvdimm_info->ppl_block_number_per_buf_pool;
	buf_pool->ppl_block_allocator = new std::queue<uint>;
	mutex_enter(&buf_pool->ppl_block_allocator_mutex);
	for (uint i = start_index + 1; i <= end_index; i++){
		buf_pool->ppl_block_allocator->push(i);
	}
	mutex_exit(&buf_pool->ppl_block_allocator_mutex);
  fprintf(stderr, "buf_pool %lu static allocator_size: %lu allocator_start: %u, allocator_end: %u\n", buf_pool->instance_no, buf_pool->ppl_block_allocator->size(), buf_pool->ppl_block_allocator->back(), buf_pool->ppl_block_allocator->front());
}

unsigned char * alloc_ppl_from_queue(buf_pool_t * buf_pool){
	mutex_enter(&buf_pool->ppl_block_allocator_mutex);
	if(buf_pool->ppl_block_allocator->empty()){
		// fprintf(stderr, "Error : static_ipl_queue is empty\n");
		mutex_exit(&buf_pool->ppl_block_allocator_mutex);
		return NULL;
	}
	// fprintf(stderr, "Static,%u\n", buf_pool->ppl_block_allocator->front());
	unsigned char * ret_address = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer, buf_pool->ppl_block_allocator->front(), nvdimm_info->each_ppl_size);
	buf_pool->ppl_block_allocator->pop();
	// fprintf(stderr, "Static,%f,%lu,%u\n", (double)(time(NULL) - my_start),buf_pool->instance_no, (nvdimm_info->ppl_block_number_per_buf_pool - buf_pool->ppl_block_allocator->size()) * 100 / nvdimm_info->ppl_block_number_per_buf_pool);
	mutex_exit(&buf_pool->ppl_block_allocator_mutex);

	// PPL Cleaner Buffer Pool Flush
	// if(srv_use_ppl_cleaner){
	// 	if(!flush_thread_started){
	// 		os_event_set(ppl_buf_flush_event);
	// 		flush_thread_started = true;
	// 	}
	// }

	// Eager Normalization
	if(buf_pool->is_eager_normalize){
		if(buf_pool->ppl_block_allocator->size() > eager_normalize_finished_threshold){
			buf_pool->is_eager_normalize = false;
		}
	}
	else{
		if (buf_pool->ppl_block_allocator->size() <= eager_normalize_started_threshold){
			fprintf(stderr, "Eager_start,%f,%lu\n", (double)(time(NULL) - my_start),buf_pool->instance_no);
			buf_pool->is_eager_normalize = true;
		}
	}

	return ret_address;
}

// 두개 할당되어 있으면 두개 return 되는지 확인 필요함
void free_ppl_and_push_queue(buf_pool_t * buf_pool, unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_static_address_to_indirection_queue Error: addr is NULL\n");
      return;
  }
	unsigned char * next_addr = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer, mach_read_from_4(addr + PPL_HDR_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
	memset_to_nvdimm(addr, 0x00, nvdimm_info->each_ppl_size);
	mutex_enter(&buf_pool->ppl_block_allocator_mutex);
	buf_pool->ppl_block_allocator->push(get_ppl_index_from_addr(nvdimm_info->ppl_start_pointer, addr, nvdimm_info->each_ppl_size));
	mutex_exit(&buf_pool->ppl_block_allocator_mutex);
	addr = next_addr;
	while(addr != NULL){
		next_addr = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer, mach_read_from_4(addr + NTH_PPL_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
		memset_to_nvdimm(addr, 0x00, nvdimm_info->each_ppl_size);
		mutex_enter(&buf_pool->ppl_block_allocator_mutex);
		buf_pool->ppl_block_allocator->push(get_ppl_index_from_addr(nvdimm_info->ppl_start_pointer, addr, nvdimm_info->each_ppl_size));
		mutex_exit(&buf_pool->ppl_block_allocator_mutex);
		addr = next_addr;
	}

	// Eager Normalization
	if(buf_pool->is_eager_normalize && (buf_pool->ppl_block_allocator->size() > eager_normalize_finished_threshold)){
		buf_pool->is_eager_normalize = false;
	}
	
	// fprintf(stderr, "free static address : %p\n", addr);
}

//ipl index를 통해 해당 ipl의 주소를 찾아준다.
unsigned char * get_addr_from_ppl_index(unsigned char * start_ptr, uint index, uint64_t ipl_per_page_size){
  if(index == 0)  return NULL; // 할당되지 않은 경우
  unsigned char * ret_addr = start_ptr + ((uint64_t)(index - 1) * ipl_per_page_size);
  return ret_addr;
}

uint get_ppl_index_from_addr(unsigned char * start_ptr, unsigned char * ret_addr, uint64_t ipl_per_page_size){
  uint64_t ret_index = (uint64_t)(ret_addr - start_ptr) / ipl_per_page_size;
  return uint(ret_index + 1);
}

#endif
