#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>



void make_static_indirection_queue(buf_pool_t * buf_pool){
  uint start_index = nvdimm_info->static_ipl_page_number_per_buf_pool * buf_pool->instance_no;
  uint end_index = start_index + nvdimm_info->static_ipl_page_number_per_buf_pool;
  mutex_enter(&buf_pool->static_allocator_mutex);
  for (uint i = start_index + 1; i <= end_index; i++){
    buf_pool->static_ipl_allocator->push(i);
  }
  mutex_exit(&buf_pool->static_allocator_mutex);
  //fprintf(stderr, "buf_pool %lu static allocator_size: %d allocator_start: %u, allocator_end: %u\n", buf_pool->instance_no, buf_pool->static_ipl_allocator->size(), buf_pool->static_ipl_allocator->back(), buf_pool->static_ipl_allocator->front());
}

unsigned char * alloc_static_address_from_indirection_queue(buf_pool_t * buf_pool){
  mutex_enter(&buf_pool->static_allocator_mutex);
  if(buf_pool->static_ipl_allocator->empty()){
      // fprintf(stderr, "Error : static_ipl_queue is empty\n");
      mutex_exit(&buf_pool->static_allocator_mutex);
      return NULL;
  }
  // fprintf(stderr, "Static,%u\n", buf_pool->static_ipl_allocator->front());
  unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, buf_pool->static_ipl_allocator->front(), nvdimm_info->static_ipl_per_page_size);
  buf_pool->static_ipl_allocator->pop();
  // fprintf(stderr, "Static,%f,%lu,%u\n", (double)(time(NULL) - start),buf_pool->instance_no, (nvdimm_info->static_ipl_page_number_per_buf_pool - buf_pool->static_ipl_allocator->size()) * 100 / nvdimm_info->static_ipl_page_number_per_buf_pool);
  mutex_exit(&buf_pool->static_allocator_mutex);
  
  return ret_address;
}

void free_static_address_to_indirection_queue(buf_pool_t * buf_pool, unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_static_address_to_indirection_queue Error: addr is NULL\n");
      return;
  }
  memset(addr, 0x00, nvdimm_info->static_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->static_ipl_per_page_size);
  mutex_enter(&buf_pool->static_allocator_mutex);
  buf_pool->static_ipl_allocator->push(get_ipl_index_from_addr(nvdimm_info->static_start_pointer, addr, nvdimm_info->static_ipl_per_page_size));
  mutex_exit(&buf_pool->static_allocator_mutex);
  // fprintf(stderr, "free static address : %p\n", addr);
}

void make_dynamic_indirection_queue(buf_pool_t * buf_pool){
  uint start_index = nvdimm_info->dynamic_ipl_page_number_per_buf_pool * buf_pool->instance_no;
  uint end_index = start_index + nvdimm_info->dynamic_ipl_page_number_per_buf_pool;
  mutex_enter(&buf_pool->dynamic_allocator_mutex);
  for (uint i = start_index + 1; i <= end_index; i++){
    buf_pool->dynamic_ipl_allocator->push(i);
  }
  fprintf(stderr, "buf_pool %lu dynamic allocator_size: %d allocator_start: %u, allocator_end: %u\n", buf_pool->instance_no, buf_pool->dynamic_ipl_allocator->size(), buf_pool->dynamic_ipl_allocator->back(), buf_pool->dynamic_ipl_allocator->front());
  mutex_exit(&buf_pool->dynamic_allocator_mutex);
}


unsigned char * alloc_dynamic_address_from_indirection_queue(buf_pool_t * buf_pool){
  mutex_enter(&buf_pool->dynamic_allocator_mutex);
  if(buf_pool->dynamic_ipl_allocator->empty()){
      // fprintf(stderr, "Error : dynamic_ipl_queue is empty\n");
      mutex_exit(&buf_pool->dynamic_allocator_mutex);
      return NULL;
  }
  // fprintf(stderr, "Dynamic,%u\n", buf_pool->dynamic_ipl_allocator->front());
  unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->dynamic_start_pointer, buf_pool->dynamic_ipl_allocator->front(), nvdimm_info->dynamic_ipl_per_page_size);
  buf_pool->dynamic_ipl_allocator->pop();
  mutex_exit(&buf_pool->dynamic_allocator_mutex);
  // fprintf(stderr,"Dynamic,%f,%lu,%u\n", (double)(time(NULL) - start), buf_pool->instance_no, (nvdimm_info->dynamic_ipl_page_number_per_buf_pool - buf_pool->dynamic_ipl_allocator->size()) * 100 / nvdimm_info->dynamic_ipl_page_number_per_buf_pool);
  return ret_address;
}

void free_dynamic_address_to_indirection_queue(buf_pool_t * buf_pool, unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_dynamic_address_to_indirection_queue Error : addr is NULL\n");
      return;
  }
  memset(addr, 0x00, nvdimm_info->dynamic_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->dynamic_ipl_per_page_size);
  mutex_enter(&buf_pool->dynamic_allocator_mutex);
  buf_pool->dynamic_ipl_allocator->push(get_ipl_index_from_addr(nvdimm_info->dynamic_start_pointer, addr, nvdimm_info->dynamic_ipl_per_page_size));
  mutex_exit(&buf_pool->dynamic_allocator_mutex);
  // fprintf(stderr, "free dyanmic_address : %p\n", addr);
}




void make_second_dynamic_indirection_queue(buf_pool_t * buf_pool){
  uint start_index = nvdimm_info->second_dynamic_ipl_page_number_per_buf_pool * buf_pool->instance_no;
  uint end_index = start_index + nvdimm_info->second_dynamic_ipl_page_number_per_buf_pool;
  mutex_enter(&buf_pool->second_dynamic_allocator_mutex);
  for (uint i = start_index + 1; i <= end_index; i++){
    buf_pool->second_dynamic_ipl_allocator->push(i);
  }
  mutex_exit(&buf_pool->second_dynamic_allocator_mutex);
  fprintf(stderr, "buf_pool %lu second dynamic allocator_size: %d allocator_start: %u, allocator_end: %u\n", buf_pool->instance_no, buf_pool->second_dynamic_ipl_allocator->size(), buf_pool->second_dynamic_ipl_allocator->back(), buf_pool->second_dynamic_ipl_allocator->front());
}


unsigned char * alloc_second_dynamic_address_from_indirection_queue(buf_pool_t * buf_pool){
  mutex_enter(&buf_pool->second_dynamic_allocator_mutex);
  if(buf_pool->second_dynamic_ipl_allocator->empty()){
      // fprintf(stderr, "Error : dynamic_ipl_queue is empty\n");
      mutex_exit(&buf_pool->second_dynamic_allocator_mutex);
      return NULL;
  }
  // fprintf(stderr, "Dynamic,%u\n", buf_pool->dynamic_ipl_allocator->front());
  unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->second_dynamic_start_pointer, buf_pool->second_dynamic_ipl_allocator->front(), nvdimm_info->second_dynamic_ipl_per_page_size);
  buf_pool->second_dynamic_ipl_allocator->pop();
  mutex_exit(&buf_pool->second_dynamic_allocator_mutex);
  // fprintf(stderr,"Second_Dynamic,%f,%lu,%u\n", (double)(time(NULL) - start), buf_pool->instance_no, (nvdimm_info->second_dynamic_ipl_page_number_per_buf_pool - buf_pool->second_dynamic_ipl_allocator->size()) * 100 / nvdimm_info->second_dynamic_ipl_page_number_per_buf_pool);
  return ret_address;
}

void free_second_dynamic_address_to_indirection_queue(buf_pool_t * buf_pool, unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_dynamic_address_to_indirection_queue Error : addr is NULL\n");
      return;
  }
  memset(addr, 0x00, nvdimm_info->second_dynamic_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->second_dynamic_ipl_per_page_size);
  mutex_enter(&buf_pool->second_dynamic_allocator_mutex);
  buf_pool->second_dynamic_ipl_allocator->push(get_ipl_index_from_addr(nvdimm_info->second_dynamic_start_pointer, addr, nvdimm_info->second_dynamic_ipl_per_page_size));
  mutex_exit(&buf_pool->second_dynamic_allocator_mutex);
  // fprintf(stderr, "free dyanmic_address : %p\n", addr);
}

//ipl index를 통해 해당 ipl의 주소를 찾아준다.
unsigned char * get_addr_from_ipl_index(unsigned char * start_ptr, uint index, uint64_t ipl_per_page_size){
  if(index == 0)  return NULL; // 할당되지 않은 경우
  unsigned char * ret_addr = start_ptr + ((index - 1) * ipl_per_page_size);
  return ret_addr;
}

uint get_ipl_index_from_addr(unsigned char * start_ptr, unsigned char * ret_addr, uint64_t ipl_per_page_size){
  uint64_t ret_index = (ret_addr - start_ptr) / ipl_per_page_size;
  return uint(ret_index + 1);
}

