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

		//fprintf(stderr, "%lu,%lu dynamic_addr: %lu\n", space_no, page_no, dynamic_addr);

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
//			ib::info() << "Static IPL page " << page_id.space() << ":" << page_id.page_no() << " need to apply"; 
			return STATIC;
		}
			// this page is IPLized page return address
//			ib::info() << "Dynamic IPL page " << page_id.space()  << ":" << page_id.page_no() << " need to apply"; 
	
		return DYNAMIC;
	}

	//ib::info() << "Normal page " << page_id.space() << ":" << page_id.page_no() << " need to apply"; 

	return NORMAL;
}

// TODO(jhpark): integrate the `set_apply_info_and_log_apply` function
// For dynamic address list, now we leave the list data structure in IPL map
void recv_ipl_apply(buf_block_t* block) {
	// step1. get the apply info
	apply_log_info apply_info;
	std::tr1::unordered_map<page_id_t, std::vector<uint64_t> >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(block->page.id);
	ulint real_size, page_lsn;
	
	if (recv_iter != ipl_recv_map.end()) {
		std::vector<uint64_t>::iterator vit = recv_iter->second.begin();
		apply_info.static_start_pointer = nvdimm_recv_ptr + *vit;

		real_size = recv_ipl_get_wp(nvdimm_recv_ptr + *vit);
		page_lsn = recv_ipl_get_lsn(nvdimm_recv_ptr + *vit);

		++vit;	
		if (vit == recv_iter->second.end()) {
			apply_info.dynamic_start_pointer = 0;	
		} else {
			uint ipl_index = mach_read_from_4(apply_info.static_start_pointer 
																				+ DYNAMIC_ADDRESS_OFFSET);
			if (ipl_index != *vit) {
				fprintf(stderr, "error!!!\n");
			}

			unsigned char* dynamic_start_pointer = 
								nvdimm_recv_ptr + nvdimm_info->static_ipl_per_page_size
								* nvdimm_info->static_ipl_max_page_count;

			apply_info.dynamic_start_pointer = 
								get_addr_from_ipl_index(dynamic_start_pointer
								, ipl_index, nvdimm_info->dynamic_ipl_per_page_size);
				
		}

		apply_info.block = block;
		apply_info.space_id = block->page.id.space();
		apply_info.page_no = block->page.id.page_no();
	
	
		// step2. copy the IPL info into memory
		mtr_t temp_mtr;
		mtr_start(&temp_mtr);
		mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);

		if (!recv_copy_log_to_mem_to_apply(&apply_info, &temp_mtr, real_size, page_lsn)) {
			ib::info() << "IPL apply error " << block->page.id.space() 
							<< ":" << block->page.id.page_no();
		} else {
#ifdef UNIV_IPL_DEBUG
			ib::info() << "IPL apply success! " << block->page.id.space() 
							<< ":" << block->page.id.page_no();
#endif 
		}

		// step3. remove IPL log from recv_ipl_map
		//ipl_recv_map.erase(block->page.id);
#ifdef UNIV_IPL_DEBUG
/*
		ib::info() << block->page.id.space() << ":" << block->page.id.page_no() 
							<< " is erased";
		recv_iter = ipl_recv_map.find(block->page.id);
		if (recv_iter == ipl_recv_map.end()) {
			ib::info() << "confirm!";
		}
*/
#endif
	}
}

// TODO(jhpark): fix the IPL log header offset; plz double check!!!
void recv_ipl_set_wp(unsigned char* ipl_ptr, uint32_t diff) {
	mach_write_to_4(ipl_ptr + (IPL_LOG_HEADER_SIZE-12), diff);
}

ulint recv_ipl_get_wp(unsigned char* ipl_ptr) {
	return mach_read_from_4(ipl_ptr + (IPL_LOG_HEADER_SIZE-12));
}

void recv_ipl_set_lsn(unsigned char* ipl_ptr, lsn_t lsn) {
	mach_write_to_8(ipl_ptr + (IPL_LOG_HEADER_SIZE-8), lsn);
}

lsn_t recv_ipl_get_lsn(unsigned char* ipl_ptr) {
	return mach_read_from_8(ipl_ptr + (IPL_LOG_HEADER_SIZE-8));
}

bool recv_copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr, ulint real_size, lsn_t page_lsn) {
	byte * apply_log_buffer;
	if (real_size == 0) return true;

	// LSN check
	ulint cur_page_lsn = mach_read_from_8(apply_info->block->frame + FIL_PAGE_LSN);
	if (cur_page_lsn >= page_lsn) {
#ifdef UNIV_NVDIMM_DEBUG
		fprintf(stderr, "[debug] cur_page_lsn %lu page_lsn %lu (%lu:%lu)\n"
									, cur_page_lsn, page_lsn, apply_info->space_id, apply_info->page_no);
#endif
		return true;
	}

	if(apply_info->dynamic_start_pointer == NULL){
		apply_log_buffer = (byte *)calloc(nvdimm_info->static_ipl_per_page_size, sizeof(char));
		memcpy(apply_log_buffer
					, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE
					, real_size - IPL_LOG_HEADER_SIZE);
					/* , nvdimm_info->static_ipl_page_size); */
	} else {

		ulint offset = 0;
		ulint apply_buffer_size = nvdimm_info->static_ipl_per_page_size 
															+ nvdimm_info->dynamic_ipl_per_page_size;
		apply_log_buffer = (byte *)calloc(apply_buffer_size, sizeof(char));

		ulint actual_static = nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE;
		ulint actual_dynamic = nvdimm_info->dynamic_ipl_per_page_size;
		
		if (real_size == 0) goto normal;

		if (real_size < nvdimm_info->static_ipl_per_page_size) {
			ib::info() << "This phase, we copy within the static IPL pages";
			actual_static = real_size;
		} else {
			ib::info() << "This phase, we copy beyond the static IPL pages";
			actual_dynamic = real_size;
		}		

normal:
		memcpy(apply_log_buffer + offset
					, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE
					, actual_static);

		offset += nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE;
		memcpy(apply_log_buffer + offset
						, apply_info->dynamic_start_pointer
						, actual_dynamic);
	}

	recv_ipl_log_apply(apply_log_buffer, apply_info, temp_mtr, real_size);
	
	// set lsn 
  fprintf(stderr, "original page_lsn: %lu new: %lu (%lu:%lu)\n"
                , mach_read_from_8(apply_info->block->frame + FIL_PAGE_LSN), page_lsn, apply_info->space_id, apply_info->page_no); 

	mach_write_to_8(apply_info->block->frame + FIL_PAGE_LSN, page_lsn);

	free(apply_log_buffer);
	return true;
}

void recv_ipl_log_apply(byte * apply_log_buffer, apply_log_info * apply_info, mtr_t * temp_mtr, ulint real_size){
  byte * start_ptr = apply_log_buffer;
  byte * end_ptr = (apply_info->dynamic_start_pointer == NULL) ? apply_log_buffer + real_size : apply_log_buffer + nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size;

  ulint now_len = 0;
  while (start_ptr < end_ptr) {
    // log_hdr를 가져와서 저장
    mlog_id_t log_type = mlog_id_t(mach_read_from_1(start_ptr));
    start_ptr += 1;
    ulint body_len = mach_read_from_2(start_ptr);
    start_ptr += 2;
    if(log_type == 0 && body_len == 0){
      now_len = start_ptr - apply_log_buffer - 3;
      goto apply_end;
    }
    // fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu\n",apply_info->space_id, apply_info->page_no, log_type, body_len);

    //log apply 진행 후, recovery 시작 위치 이동.
    recv_parse_or_apply_log_rec_body(log_type, start_ptr, start_ptr + body_len, apply_info->space_id, apply_info->page_no, apply_info->block, temp_mtr);
    start_ptr += body_len;
  }
  now_len = start_ptr - apply_log_buffer;
apply_end:
  now_len += IPL_LOG_HEADER_SIZE;
  apply_info->block->page.ipl_write_pointer = apply_info->block->page.static_ipl_pointer + now_len;
  temp_mtr->discard_modifications();
  mtr_commit(temp_mtr);
}

void recv_clean_ipl_map() {
	for (std::tr1::unordered_map<page_id_t, std::vector<uint64_t> >::iterator it = ipl_recv_map.begin(); it != ipl_recv_map.end(); ++it) {
		ipl_recv_map.erase(it);
	}	
	ib::info() << "IPL mapping region is cleand!";
}
