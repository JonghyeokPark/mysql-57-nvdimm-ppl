#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

void make_static_indirection_queue(unsigned char * static_start_pointer, uint64_t static_ipl_size, uint static_ipl_max_page_count){
  for (uint i = 0; i < static_ipl_max_page_count; i++){
    static_ipl_queue.push(static_start_pointer + (i * static_ipl_size));
  }
  fprintf(stderr, "static_ipl_queue size : %d\n", static_ipl_queue.size());
  
}

void make_dynamic_indirection_queue(unsigned char * dynamic_start_pointer, uint64_t dynamic_ipl_size, uint dynamic_ipl_max_page_count){
  for(uint i = 0; i < dynamic_ipl_max_page_count; i++){
    dynamic_ipl_queue.push(dynamic_start_pointer + (i * dynamic_ipl_size));
  }
  fprintf(stderr, "dynamic_ipl_queue size : %d\n", dynamic_ipl_queue.size());
}