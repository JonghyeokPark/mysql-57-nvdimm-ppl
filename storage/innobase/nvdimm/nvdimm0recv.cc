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

/*
	// Demisifying IPL source code 

	// [ IPL header ] [ IPL Data ] => IPL static page size: 1KB , dynamic size: 8KB
	// [ Space ID / Page ID / Dynamic Address ] => Dyanamic adress is 4KB or 8KB ? 

	// TODO(jhpark): check!!! 
	// [Space ID / Page ID / Length / Dynamic Address ] => total 16B
	
	// Rule1. All IPL page has static region
*/

// key: page_id, value: IPL address offset
std::tr1::unordered_map<page_id_t, std::vector<uint64_t> > ipl_recv_map;

// anlayze the IPL region for reconstructing 
void recv_ipl_parse_log() {

	fprintf(stderr, "[DEBUG] printf_nvidmm_info: static_page_size: %lu dynamic_page_size: %lu\n"
			 , nvdimm_info->static_ipl_per_page_size
			 , nvdimm_info->dynamic_ipl_per_page_size);	

	uint64_t space_no = -1, page_no = -1, dynamic_addr= -1;
	nvdimm_recv_ptr = nvdimm_ptr + (2*1024*1024*1024UL);

	// step1. Read the IPL region from the begining of NVDIMM
	byte hdr[IPL_LOG_HEADER_SIZE];
	for (uint64_t i = 0; i < nvdimm_info->static_ipl_size; i+= nvdimm_info->static_ipl_per_page_size) {
		// step2. Get the header information
		memcpy(hdr, nvdimm_recv_ptr + i, IPL_LOG_HEADER_SIZE);
		space_no = mach_read_from_4(hdr);
		page_no = mach_read_from_4(hdr + 4);
		dynamic_addr = mach_read_from_4(hdr + 8);

		fprintf(stderr, "%lu,%lu dynamic_addr: %lu\n", space_no, page_no, dynamic_addr);

		ipl_recv_map[page_id_t(space_no, page_no)].push_back(i);

		// step3. Get the dyanmic address
		if (dynamic_addr != 0) {
			// this page has also dynamic IPL too
			ipl_recv_map[page_id_t(space_no, page_no)].push_back(dynamic_addr);
		}

	}	
}

void recv_ipl_map_print() {

	for (std::tr1::unordered_map<page_id_t, std::vector<uint64_t> >::iterator it = ipl_recv_map.begin(); it != ipl_recv_map.end(); ++it) {

		fprintf(stderr, "(%u,%u) : \n", it->first.space(), 
																			it->first.page_no());

		for (std::vector<uint64_t>::iterator vit = it->second.begin(); vit != it->second.end(); ++vit) {
   		fprintf(stderr, "addr: %lu\n", *vit); 
		}
	}
}

RECV_IPL_PAGE_TYPE recv_check_iplized(page_id_t page_id) {
	std::tr1::unordered_map<page_id_t, std::vector<uint64_t> >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(page_id);
	if (recv_iter != ipl_recv_map.end()) {
		std::vector<uint64_t>::iterator vit = recv_iter->second.begin();
		++vit;
		if (vit == recv_iter->second.end()) {
			// this page is IPLized page return address
			ib::info() << "Static IPL page " << page_id.space() << page_id.page_no() << " need to apply"; 
			return STATIC;
		}
			// this page is IPLized page return address
			ib::info() << "Dynamic IPL page " << page_id.space() << page_id.page_no() << " need to apply"; 
	
		return DYNAMIC;
	}

	ib::info() << "Normal page " << page_id.space() << page_id.page_no() << " need to apply"; 

	return NORMAL;
}

// TODO(jhpark): integrate the `set_apply_info_and_log_apply` function
void recv_ipl_apply(buf_block_t* block) {
	// step1. get the apply info
	apply_log_info apply_info;
	std::tr1::unordered_map<page_id_t, std::vector<uint64_t> >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(block->page.id);
	
	if (recv_iter != ipl_recv_map.end()) {
		std::vector<uint64_t>::iterator vit = recv_iter->second.begin();
		apply_info.static_start_pointer = nvdimm_recv_ptr + *vit;
		++vit;	
		if (vit == recv_iter->second.end()) {
			apply_info.dynamic_start_pointer = 0;	
		} else {
			apply_info.dynamic_start_pointer = nvdimm_recv_ptr + *vit;		
		}

		apply_info.block = block;
		apply_info.space_id = block->page.id.space();
		apply_info.page_no = block->page.id.page_no();
	}
	
	// step2. copy the IPL info into memory
	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	if (!copy_log_to_mem_to_apply(&apply_info, &temp_mtr)) {
		ib::info() << "IPL apply error " << block->page.id.space() 
							<< ":" << block->page.id.page_no();
	} else {
		ib::info() << "IPL apply success! " << block->page.id.space() 
							<< ":" << block->page.id.page_no();
	}

}

