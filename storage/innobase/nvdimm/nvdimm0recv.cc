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
#include <tr1/unordered_map>

// NVDIMM IPL_MAP
/*
	IPL_INFO {
		unsigned char* static_region_pointer;
		unsigned char* dynamic_region_pointer;
		ulint page_ipl_region_size;
	}
*/

std::tr1::unordered_map<page_id_t, ipl_info * > ipl_recv_map;

// anlayze the IPL region for reconstructing 
void recv_ipl_parse_log() {

	fprintf(stderr, "[DEBUG] printf_nvidmm_info: static_page_size: %lu dynamic_page_size: %lu\n", nvdimm_info->static_ipl_per_page_size
			 , nvdimm_info->dynamic_ipl_per_page_size);	

	// step1. Read the IPL region from the begining
	
	// step2. Construct the IPL information

	// step3. Add the ipl_recv_map
}
