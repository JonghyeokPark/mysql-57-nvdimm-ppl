#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

std::queue<uint> static_ipl_queue;
std::queue<uint> dynamic_ipl_queue;

void make_static_indirection_queue(unsigned char * static_start_pointer, uint64_t static_ipl_per_page_size, uint static_ipl_max_page_count){
  mutex_enter(&nvdimm_info->static_region_mutex);
  for (uint i = 1; i <= static_ipl_max_page_count; i++){
    static_ipl_queue.push(i);
  }
  fprintf(stderr, "static_ipl_queue size : %d\n", static_ipl_queue.size());
  mutex_exit(&nvdimm_info->static_region_mutex);
}

unsigned char * alloc_static_address_from_indirection_queue(){
  mutex_enter(&nvdimm_info->static_region_mutex);
  if(static_ipl_queue.empty()){
      // fprintf(stderr, "Error : static_ipl_queue is empty\n");
      mutex_exit(&nvdimm_info->static_region_mutex);
      return NULL;
  }
  unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, static_ipl_queue.front(), nvdimm_info->static_ipl_per_page_size);
  static_ipl_queue.pop();
  // fprintf(stderr, "static_ipl usage : %d\n", nvdimm_info->static_ipl_max_page_count - static_ipl_queue.size());
  mutex_exit(&nvdimm_info->static_region_mutex);
  return ret_address;
}

void free_static_address_to_indirection_queue(unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_static_address_to_indirection_queue Error: addr is NULL\n");
      return;
  }
  mutex_enter(&nvdimm_info->static_region_mutex);
  memset(addr, 0x00, nvdimm_info->static_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->static_ipl_per_page_size);
  static_ipl_queue.push(get_ipl_index_from_addr(nvdimm_info->static_start_pointer, addr, nvdimm_info->static_ipl_per_page_size));
  // fprintf(stderr, "free static address : %p\n", addr);
  mutex_exit(&nvdimm_info->static_region_mutex);
}




void make_dynamic_indirection_queue(unsigned char * dynamic_start_pointer, uint64_t dynamic_ipl_per_page_size, uint dynamic_ipl_max_page_count){
  mutex_enter(&nvdimm_info->dynamic_region_mutex);
  for(uint i = 1; i <= dynamic_ipl_max_page_count; i++){
    dynamic_ipl_queue.push(i);
  }
  fprintf(stderr, "dynamic_ipl_queue size : %d\n", dynamic_ipl_queue.size());
  mutex_exit(&nvdimm_info->dynamic_region_mutex);
}


unsigned char * alloc_dynamic_address_from_indirection_queue(){
  mutex_enter(&nvdimm_info->dynamic_region_mutex);
  if(dynamic_ipl_queue.empty()){
      // fprintf(stderr, "Error : dynamic_ipl_queue is empty\n");
      mutex_exit(&nvdimm_info->dynamic_region_mutex);
      return NULL;
  }
  unsigned char * ret_address = get_addr_from_ipl_index(nvdimm_info->dynamic_start_pointer, dynamic_ipl_queue.front(), nvdimm_info->dynamic_ipl_per_page_size);
  dynamic_ipl_queue.pop();
  // fprintf(stderr, "dynamic_ipl usage: %d\n", nvdimm_info->dynamic_ipl_max_page_count - dynamic_ipl_queue.size());
  mutex_exit(&nvdimm_info->dynamic_region_mutex);
  return ret_address;
}

void free_dynamic_address_to_indirection_queue(unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_dynamic_address_to_indirection_queue Error : addr is NULL\n");
      return;
  }
  mutex_enter(&nvdimm_info->dynamic_region_mutex);
  memset(addr, 0x00, nvdimm_info->dynamic_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->dynamic_ipl_per_page_size);
  // fprintf(stderr, "free dyanmic_address : %p\n", addr);
  dynamic_ipl_queue.push(get_ipl_index_from_addr(nvdimm_info->dynamic_start_pointer, addr, nvdimm_info->dynamic_ipl_per_page_size));
  mutex_exit(&nvdimm_info->dynamic_region_mutex);
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

