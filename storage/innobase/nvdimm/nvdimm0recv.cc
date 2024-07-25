// Copyright 2023 VLDB Lab. (http://vldb.skku.ac.kr/)
// Author: Jonghyeok Park
// E-mail: jonghyeok.park@hufs.ac.kr
#ifdef UNIV_NVDIMM_PPL
#include "nvdimm-ipl.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <tr1/unordered_map>

// key: page_id, value: IPL address offset
std::tr1::unordered_map<page_id_t, uint64_t> ipl_recv_map;

// anlayze the IPL region for reconstructing 
void recv_ipl_parse_log() {

	fprintf(stderr, "[DEBUG] printf_nvidmm_info: static_page_size: %lu\n"
			 , nvdimm_info->each_ppl_size);	

	byte first_ppl_marker = 0;
	uint64_t space_no = -1, page_no = -1, dynamic_addr= -1;
	nvdimm_recv_ptr = nvdimm_ptr;

	// step1. Read the IPL region from the begining of NVDIMM
	byte hdr[PPL_BLOCK_HDR_SIZE];
	for (uint64_t i = 0; i < nvdimm_info->overall_ppl_size; i+= nvdimm_info->each_ppl_size) {
		// step2. Get the header information
		memcpy(hdr, nvdimm_recv_ptr + i, PPL_HDR_FIRST_MARKER);
		first_ppl_marker = mach_read_from_1(hdr + PPL_HDR_FIRST_MARKER);
		if(!first_ppl_marker){
			continue;
		}
		space_no = mach_read_from_4(hdr + IPL_HDR_SPACE);
		page_no = mach_read_from_4(hdr + IPL_HDR_PAGE);
	
		// skip deleted IPL log
		// SIPL > DIPL > SDIPL 작성된 IPL 로그가 있는 page가 normalize 된 경우, 
		// SDIPL > DIPL > SIPL 순으로, 삭제되기 때문에 normalize가 되어도 SDIPL이 있을 수 있다.
		// 따라서, normalize flag를 확인해서 IPLed 페이지 해제함.
    /*
		if(get_flag(nvdimm_recv_ptr + i + IPL_HDR_FLAG, NORMALIZE)) {
			fprintf(stderr, "(%u,%u) is ipled but become normailze at crash\n"
							, space_no, page_no);
			continue;
		}
    */

		// PPL Page 출력해보기
		// uint64_t space, page_no, ppl_length, page_lsn;
		// unsigned char * dynamic_pointer, * second_dynamic_pointer;
		// dynamic_pointer = NULL;
		// second_dynamic_pointer = NULL;
		// space = mach_read_from_4(hdr);
		// page_no = mach_read_from_4(hdr + PAGE_NO_OFFSET);
		// page_lsn = get_page_lsn_from_ppl_header(hdr);
		// dynamic_pointer = get_addr_from_ppl_index(nvdimm_info->dynamic_start_pointer, 
		// 											mach_read_from_4(hdr + DYNAMIC_ADDRESS_OFFSET), 
		// 											nvdimm_info->dynamic_ipl_per_page_size);
		// if(dynamic_pointer != NULL) {
		// 	second_dynamic_pointer = get_addr_from_ppl_index(nvdimm_info->second_dynamic_start_pointer, 
		// 											mach_read_from_4(dynamic_pointer), 
		// 											nvdimm_info->second_dynamic_ipl_per_page_size);
		// }
		// ppl_length = mach_read_from_4(hdr+PPL_HDR_LEN);
		// fprintf(stderr,"PPL_INFO,%lu,%lu,%d,%d,%zu\n", space, ppl_length, (dynamic_pointer != NULL), (second_dynamic_pointer != NULL), page_lsn);

// 
		ipl_recv_map[page_id_t(space_no, page_no)] = i;
	}
}

void recv_ipl_map_print() {

	for (std::tr1::unordered_map<page_id_t, uint64_t >::iterator it = ipl_recv_map.begin(); 
			it != ipl_recv_map.end(); ++it) {

		fprintf(stderr, "(%u,%u) : %u state: %d\n"
									, it->first.space(), it->first.page_no(), it->second
                  , recv_check_iplized(it->first));

	}
}

RECV_IPL_PAGE_TYPE recv_check_iplized(page_id_t page_id) {
  
	// length로 dipl있는지 확인.
	ulint real_size, ipl_index;
	std::tr1::unordered_map<page_id_t, uint64_t >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(page_id);
	if (recv_iter != ipl_recv_map.end()) {
		uint64_t addr = recv_iter->second;
		real_size = recv_ipl_get_len(nvdimm_recv_ptr + addr);
		ipl_index = mach_read_from_4(nvdimm_recv_ptr + addr 
																				+ PPL_HDR_DYNAMIC_INDEX);

		unsigned char* dynamic_start_pointer = get_addr_from_ppl_index(
										nvdimm_info->dynamic_start_pointer
										, ipl_index
										, nvdimm_info->dynamic_ipl_per_page_size);

		unsigned char* second_dynamic_start_pointer = get_addr_from_ppl_index(
										nvdimm_info->second_dynamic_start_pointer
										, ipl_index
										, nvdimm_info->second_dynamic_ipl_per_page_size);


		
		if (second_dynamic_start_pointer != NULL) return SDIPL;
		else if (second_dynamic_start_pointer != NULL) return DIPL;
		else return SIPL;
	}
	return NORMAL;
}

// TODO(anonymous): integrate the `set_apply_info_and_log_apply` function
// For dynamic address list, now we leave the list data structure in IPL map
void recv_ipl_apply(buf_block_t* block) {
	// step1. get the apply info
	apply_log_info apply_info;
	std::tr1::unordered_map<page_id_t, uint64_t >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(block->page.id);
	ulint real_size, page_lsn;
	
	if (recv_iter != ipl_recv_map.end()) {
		uint64_t addr = recv_iter->second;

		// static ipl address and length
		apply_info.static_start_pointer = nvdimm_recv_ptr + addr;		
		apply_info.ipl_log_length = mach_read_from_4(nvdimm_recv_ptr + addr + PPL_HDR_LEN);

		page_lsn = recv_ipl_get_lsn(nvdimm_recv_ptr + addr);
		real_size = recv_ipl_get_wp(nvdimm_recv_ptr + addr);
		ib::info() << "real size: " << real_size << " "  << "length: " << apply_info.ipl_log_length
					<< " page_lsn: " << page_lsn
          << " " << block->page.id.space() << ":" << block->page.id.page_no() ;

		// debug
		if (real_size != apply_info.ipl_log_length) {
			apply_info.ipl_log_length = real_size;
		}
		// dyanmic ipl address
		uint ipl_index = mach_read_from_4(apply_info.static_start_pointer 
																				+ PPL_HDR_DYNAMIC_INDEX);

		unsigned char* dynamic_start_pointer = get_addr_from_ppl_index(
										nvdimm_info->dynamic_start_pointer
										, ipl_index
										, nvdimm_info->dynamic_ipl_per_page_size);

		apply_info.dynamic_start_pointer = dynamic_start_pointer;

		if (ipl_index == 0) {
			ib::info() << "dynamic address is zero! " 
          << block->page.id.space() << ":" << block->page.id.page_no() ;
		}

		// second dynamic ipl address

		ipl_index = mach_read_from_4(apply_info.dynamic_start_pointer);	
		unsigned char* second_dynamic_start_pointer = get_addr_from_ppl_index(
										nvdimm_info->second_dynamic_start_pointer
										, ipl_index
										, nvdimm_info->second_dynamic_ipl_per_page_size);

		apply_info.second_dynamic_start_pointer = second_dynamic_start_pointer;
		apply_info.block = block;
	
		// step2. copy the IPL info into memory
		mtr_t temp_mtr;
		mtr_start(&temp_mtr);
		mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);

		if (!recv_copy_log_to_mem_to_apply(&apply_info, &temp_mtr, real_size, page_lsn)) {
			ib::info() << "IPL apply error " << block->page.id.space() 
							<< ":" << block->page.id.page_no();
		} else {
			ib::info() << "IPL apply success! " << block->page.id.space() 
							<< ":" << block->page.id.page_no();
		}
	}
}

void recv_ipl_set_lsn(unsigned char* ipl_ptr, lsn_t lsn) {
	mach_write_to_8(ipl_ptr + PPL_HDR_LSN, lsn);
}

lsn_t recv_ipl_get_lsn(unsigned char* ipl_ptr) {
	return mach_read_from_8(ipl_ptr + PPL_HDR_LSN);
}

void recv_ipl_set_len(unsigned char* ipl_ptr, uint32_t len) {
	mach_write_to_4(ipl_ptr + PPL_HDR_LEN, len);
}

uint32_t recv_ipl_get_len(unsigned char* ipl_ptr) {
	return mach_read_from_4(ipl_ptr + PPL_HDR_LEN);
}

void recv_ipl_set_flag(unsigned char* ipl_ptr, char flag) {
	mach_write_to_1(ipl_ptr + PPL_HDR_LEN, flag);
}

char recv_ipl_get_flag(unsigned char* ipl_ptr) {
	return mach_read_from_1(ipl_ptr + PPL_HDR_NORMALIZE_MARKER);
}

void recv_ipl_set_wp(unsigned char* ipl_ptr, uint32_t cur_len) {
	mach_write_to_1(ipl_ptr + PPL_HDR_NORMALIZE_MARKER, cur_len);
}

ulint recv_ipl_get_wp(unsigned char* ipl_ptr) {
	return mach_read_from_1(ipl_ptr + PPL_HDR_NORMALIZE_MARKER);
}


bool recv_copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr, ulint real_size, lsn_t page_lsn) {

	byte * apply_log_buffer;
	ulint offset, sipl_size, dipl_size, sdipl_size, apply_buffer_size;

	if (real_size == 0) return true;

	// LSN check
	// IPLed page disk로 flush된 이후로 update가 발생한 것이므로, 
	// redo log apply 해서 적용하면 됨.
	uint64_t cur_page_lsn = mach_read_from_8(apply_info->block->frame + FIL_PAGE_LSN);
	if (cur_page_lsn >= page_lsn && page_lsn !=0) {
		fprintf(stderr, "[debug] cur_page_lsn %llu page_lsn %llu (%lu:%lu)\n"
									, cur_page_lsn, page_lsn
                  , apply_info->block->page.id.space()
                  , apply_info->block->page.id.page_no());
		return true;
	}

	offset = 0;
	apply_buffer_size = 0;
	sipl_size = nvdimm_info->each_ppl_size - PPL_BLOCK_HDR_SIZE;
	dipl_size = nvdimm_info->dynamic_ipl_per_page_size - NTH_PPL_BLOCK_HEADER_SIZE;

	// step1. second DIPL, first DIPL, SIPL apply인지 판별
	// if(apply_info->second_dynamic_start_pointer != NULL) {
	if (apply_info->ipl_log_length > 
			nvdimm_info->each_ppl_size + nvdimm_info->dynamic_ipl_per_page_size) {

		ib::info() << apply_info->block->page.id.space() << ":" << apply_info->block->page.id.page_no()
							<< " SDIPL!!!";

		// step2. SIPL, first DIPL, second DIPL 순서로 copy 연결
		sdipl_size = apply_info->ipl_log_length 
								- nvdimm_info->each_ppl_size 
								- nvdimm_info->dynamic_ipl_per_page_size;

		apply_buffer_size = sipl_size + dipl_size + sdipl_size;

		unsigned char temp_mtr_buffer [apply_buffer_size] = {NULL, };
		memcpy(temp_mtr_buffer, apply_info->static_start_pointer + PPL_BLOCK_HDR_SIZE ,sipl_size);
		offset += sipl_size;
		
		memcpy(temp_mtr_buffer + offset
					, apply_info->dynamic_start_pointer + NTH_PPL_BLOCK_HEADER_SIZE
					, dipl_size);
		offset += dipl_size;

		memcpy(temp_mtr_buffer + offset, apply_info->second_dynamic_start_pointer, sdipl_size);
	
		fprintf(stderr,"2nd DIPL org: %llu page_lsn: %llu (%lu:%lu)\n", cur_page_lsn, page_lsn
                  , apply_info->block->page.id.space()
                  , apply_info->block->page.id.page_no());

	
		// step4. IPL log apply
		recv_ipl_log_apply(temp_mtr_buffer
											, temp_mtr_buffer + apply_buffer_size
											, apply_info, temp_mtr);	 	 

		mach_write_to_8(apply_info->block->frame + FIL_PAGE_LSN, page_lsn);

		return true;
	
	} else if (apply_info->ipl_log_length > nvdimm_info->each_ppl_size) {
dipl_apply:	
		// step2. SIPL, 1st DIPL 순서대로 copy 후 연결
		dipl_size = apply_info->ipl_log_length - nvdimm_info->each_ppl_size;
		apply_buffer_size = sipl_size + dipl_size;

		unsigned char temp_mtr_buffer [apply_buffer_size] = {NULL, };
		memcpy(temp_mtr_buffer, apply_info->static_start_pointer + PPL_BLOCK_HDR_SIZE ,sipl_size);
		offset += sipl_size;
		memcpy(temp_mtr_buffer + offset
						, apply_info->dynamic_start_pointer + NTH_PPL_BLOCK_HEADER_SIZE
						, dipl_size);
	
		fprintf(stderr,"DIPL org: %llu page_lsn: %llu (%lu:%lu)\n", cur_page_lsn, page_lsn
                  , apply_info->block->page.id.space()
                  , apply_info->block->page.id.page_no());
	
		// step4. IPL log apply
		recv_ipl_log_apply(temp_mtr_buffer
											, temp_mtr_buffer + apply_buffer_size
											, apply_info, temp_mtr);	 	 


		mach_write_to_8(apply_info->block->frame + FIL_PAGE_LSN, page_lsn);
		return true;

	} else {	
		// this is SIPL only log apply; very simple one
sipl_apply:
		// step 3. IPL log apply

		sipl_size = apply_info->ipl_log_length - PPL_BLOCK_HDR_SIZE;

/*
		// (anonymous): this is 2nd DIPL case, but we use only real_size
		if (real_size > (sipl_size + dipl_size)) {
			// sipl + dipl + within the 2nd dipl area
			sdipl_size = real_size;
		} else if (real_size > sipl_size) {
			// sipl + within the dipl area
			dipl_size = real_size;
		} else if (real_size < sipl_size) {
			// within the sipl area
			sipl_size = real_size - PPL_BLOCK_HDR_SIZE;
		} else {
			ib::info() << "error!";
		}
*/

		// 만약, DIPL(?) 영역을 할당만 받고, 헤더만 작성한 경우 SIPL apply로 전환
		if (dipl_size > nvdimm_info->dynamic_ipl_per_page_size) {
			fprintf(stderr, "rare case ?!\n");
			return true;
		}

		// TODO(anonymous): do we still need this?
		fprintf(stderr,"SIPL org: %llu page_lsn: %llu (%lu:%lu)\n", cur_page_lsn, page_lsn
                  , apply_info->block->page.id.space()
                  , apply_info->block->page.id.page_no());


		recv_ipl_log_apply(apply_info->static_start_pointer + PPL_BLOCK_HDR_SIZE
											, apply_info->static_start_pointer + PPL_BLOCK_HDR_SIZE + sipl_size
											, apply_info, temp_mtr);
						
	}

	mach_write_to_8(apply_info->block->frame + FIL_PAGE_LSN, page_lsn);
	return true;
}

void recv_ipl_log_apply(byte * start_ptr, byte * end_ptr
												, apply_log_info * apply_info, mtr_t * temp_mtr){

	byte* apply_ptr = start_ptr;
	page_id_t page_id = apply_info->block->page.id;
	while (apply_ptr < end_ptr) {

		// step1. log type 읽기
		mlog_id_t log_type = mlog_id_t(mach_read_from_1(start_ptr));
    start_ptr += 1;
	
		// step2. log body length 읽기
    ulint body_len = mach_read_from_2(start_ptr);
    start_ptr += 2;

		// step3. trx id 읽기
		uint64_t trx_id = mach_read_from_8(apply_ptr);
		apply_ptr += 8;

		ib::info() << "apply while loop" << page_id.space() << ":" << page_id.page_no()
								<<  " trx-id: " << trx_id;

		recv_parse_or_apply_log_rec_body(log_type, apply_ptr, apply_ptr + body_len
																		, page_id.space(), page_id.page_no()
																		, apply_info->block, temp_mtr);

		apply_ptr += body_len;
	}

	ib::info() << "apply while loop end" << page_id.space() << ":" << page_id.page_no();

	// step4. IPL apply 완료 후, apply_ptr 기준으로 write_ptr 재설정
	//if (apply_info->second_dynamic_start_pointer != NULL) {
	if (apply_info->ipl_log_length >
			nvdimm_info->each_ppl_size + nvdimm_info->dynamic_ipl_per_page_size) {
		apply_info->block->page.ppl_write_pointer =
					apply_info->second_dynamic_start_pointer 
					+ (apply_info->ipl_log_length 
							- nvdimm_info->each_ppl_size
							- nvdimm_info->dynamic_ipl_per_page_size);
	} else if (apply_info->ipl_log_length 
						> nvdimm_info->each_ppl_size){
		apply_info->block->page.ppl_write_pointer = 
					apply_info->dynamic_start_pointer 
					+ (apply_info->ipl_log_length - nvdimm_info->each_ppl_size);
	} else {
		apply_info->block->page.ppl_write_pointer = 
					apply_info->static_start_pointer 
					+ apply_info->ipl_log_length;
	}

	temp_mtr->discard_modifications();
	mtr_commit(temp_mtr);
}

void recv_clean_ipl_map() {
	for (std::tr1::unordered_map<page_id_t, uint64_t >::iterator it = ipl_recv_map.begin(); 
			it != ipl_recv_map.end(); ++it) {
		ipl_recv_map.erase(it);
	}	
	ib::info() << "IPL mapping region is cleand!";
}


bool recv_check_normal_flag(buf_block_t* block) {
	std::tr1::unordered_map<page_id_t, uint64_t >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(block->page.id);
	uint64_t addr = recv_iter->second;
	return get_normalize_flag_in_ppl_header(nvdimm_recv_ptr + addr + PPL_HDR_NORMALIZE_MARKER);
}

lsn_t recv_get_first_ipl_lsn(buf_block_t* block) {
	std::tr1::unordered_map<page_id_t, uint64_t >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(block->page.id);
	uint64_t addr = recv_iter->second;
	return recv_ipl_get_lsn(nvdimm_recv_ptr + addr);
}

bool recv_check_normal_flag_using_page_id(page_id_t page_id) {
	std::tr1::unordered_map<page_id_t, uint64_t >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(page_id);
	uint64_t addr = recv_iter->second;
	return get_normalize_flag_in_ppl_header(nvdimm_recv_ptr + addr + PPL_HDR_NORMALIZE_MARKER);
}

lsn_t recv_get_first_ipl_lsn_using_page_id(page_id_t page_id) {
	std::tr1::unordered_map<page_id_t, uint64_t >::iterator recv_iter;
	recv_iter = ipl_recv_map.find(page_id);
	uint64_t addr = recv_iter->second;
	return recv_ipl_get_lsn(nvdimm_recv_ptr + addr);
}
#endif