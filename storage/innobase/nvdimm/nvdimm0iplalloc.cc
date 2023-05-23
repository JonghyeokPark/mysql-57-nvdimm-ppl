#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

std::queue<unsigned char *> static_ipl_queue;
std::queue<unsigned char *> dynamic_ipl_queue;

void make_static_indirection_queue(unsigned char * static_start_pointer, uint64_t static_ipl_per_page_size, uint static_ipl_max_page_count){
  mutex_enter(&nvdimm_info->static_region_mutex);
  for (uint i = 0; i < static_ipl_max_page_count; i++){
    static_ipl_queue.push(static_start_pointer + (i * static_ipl_per_page_size));
  }
  fprintf(stderr, "static_ipl_queue size : %d\n", static_ipl_queue.size());
  mutex_exit(&nvdimm_info->static_region_mutex);
}

unsigned char * alloc_static_address_from_indirection_queue(){
  mutex_enter(&nvdimm_info->static_region_mutex);
  if(static_ipl_queue.empty()){
      fprintf(stderr, "Error : static_ipl_queue is empty\n");
      mutex_exit(&nvdimm_info->static_region_mutex);
      return NULL;
  }
  unsigned char * ret = static_ipl_queue.front();
  static_ipl_queue.pop();
  fprintf(stderr, "Available static region : %u\n", static_ipl_queue.size());
  mutex_exit(&nvdimm_info->static_region_mutex);
  return ret;
}

bool free_static_address_to_indirection_queue(unsigned char * addr){
  if(addr == NULL){
      fprintf(stderr, "free_static_address_to_indirection_queue Error: addr is NULL\n");
      return true;
  }
  mutex_enter(&nvdimm_info->static_region_mutex);
  memset(addr, 0x00, nvdimm_info->static_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->static_ipl_per_page_size);
  static_ipl_queue.push(addr);
  // fprintf(stderr, "free static address : %p\n", addr);
  mutex_exit(&nvdimm_info->static_region_mutex);
  return true;
}




void make_dynamic_indirection_queue(unsigned char * dynamic_start_pointer, uint64_t dynamic_ipl_per_page_size, uint dynamic_ipl_max_page_count){
  mutex_enter(&nvdimm_info->dynamic_region_mutex);
  for(uint i = 0; i < dynamic_ipl_max_page_count; i++){
    dynamic_ipl_queue.push(dynamic_start_pointer + (i * dynamic_ipl_per_page_size));
  }
  fprintf(stderr, "dynamic_ipl_queue size : %d\n", dynamic_ipl_queue.size());
  mutex_exit(&nvdimm_info->dynamic_region_mutex);
}


unsigned char * alloc_dynamic_address_from_indirection_queue(){
  mutex_enter(&nvdimm_info->dynamic_region_mutex);
  if(dynamic_ipl_queue.empty()){
      fprintf(stderr, "Error : dynamic_ipl_queue is empty\n");
      mutex_exit(&nvdimm_info->dynamic_region_mutex);
      return NULL;
  }
  unsigned char * ret = dynamic_ipl_queue.front();
  dynamic_ipl_queue.pop();
  fprintf(stderr, "Available Dynamic region : %u\n", dynamic_ipl_queue.size());
  mutex_exit(&nvdimm_info->dynamic_region_mutex);
  return ret;
}

bool free_dynamic_address_to_indirection_queue(unsigned char * addr){
  if(addr == NULL){
      // fprintf(stderr, "free_dynamic_address_to_indirection_queue Error : addr is NULL\n");
      return true;
  }
  mutex_enter(&nvdimm_info->dynamic_region_mutex);
  memset(addr, 0x00, nvdimm_info->dynamic_ipl_per_page_size);
  flush_cache(addr, nvdimm_info->dynamic_ipl_per_page_size);
  // fprintf(stderr, "free dyanmic_address : %p\n", addr);
  dynamic_ipl_queue.push(addr);
  mutex_exit(&nvdimm_info->dynamic_region_mutex);
  return true;
}

