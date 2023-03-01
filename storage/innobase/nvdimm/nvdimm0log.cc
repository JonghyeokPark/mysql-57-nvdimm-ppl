// E-mail: akindo19@skku.edu
// Author: Jonghyeok Park
// E-mail: akindo19@skku.edu

// TODO(jhpark): Implement Log applier
// 1. REDO Log based log applier (Merge)
// 2. Convert LSN-based Redo log -> Per-page Redo log
// 3. Utilize Change buffer "merge" operation (`ibuf_merge_or_delete_for_page()`)
// 	- when index page is read from a disk to buffer pool, 
//	this function applies any buffered operations to the page and deletes the entries from the change buffer
// 	how to handle doropped pages ( operation is NOT read but page is created ) ??

// 	At least, we may know the appropriate position for perfect timing for merge operation
//  + Get the idea from data structures to managin per-page log

// Log applier is actually identical to recovery process
// Call recv_recover_page() function on the fly in normal process  ... ? 

// step1. collect/manage pages's modification into NVDIMM region per-page manner
// step2. apply per-page log when 
// step3. for split pages do not invoke IPLization process

#include "nvdimm-ipl.h"
#include "mtr0log.h"
#include "page0page.h"
#include <map>

using namespace std;

void insert_new_ipl_info(page_id_t page_id){
	ipl_info new_ipl_info;
	new_ipl_info.ipl_start_offset = nvdimm_offset;
	new_ipl_info.ipl_write_pointer = 0;
	new_ipl_info.have_to_flush = false;
	ipl_map.insert(make_pair(page_id, new_ipl_info));
}

void nvdimm_ipl_initialize() {
	// TODO(jhpark): initialize IPL-related data structures 
	// For now, we use static allocation scheme.
	// We will support dynamic mapping for IPL region
	NVDIMM_INFO_PRINT("NVDIMM IPL Region Initialization!\n");
}

bool nvdimm_ipl_add(const page_id_t page_id, unsigned char *log, ulint len, mlog_id_t type) {
	// step1. get offset in NVDIMM IPL region from ipl_map table

	if (nvdimm_offset >= ULONG_MAX) {
		std::cerr << "we need more ... NVDIMMM region\n";
		exit(1);
	}

	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	
	map<page_id_t, ipl_info>::iterator it = ipl_map.find(page_id);
	uint64_t ipl_start_offset = 0;
	uint64_t ipl_write_pointer = 0;


	if (it == ipl_map.end()) {
		// - First write
		insert_new_ipl_info(page_id);
		ipl_start_offset = nvdimm_offset;
		nvdimm_offset += IPL_LOG_REGION_SZ;
		ipl_write_pointer = 0;
	} else {
		ipl_start_offset = it->second.ipl_start_offset;
		ipl_write_pointer = it->second.ipl_write_pointer;
	}


	// step2. check capacity 
	if (ipl_write_pointer + len + sizeof(IPL_LOG_HDR) >= IPL_LOG_REGION_SZ) {
    	fprintf(stderr, "Cannot Save the log, Add the Flush (%u, %u)\n", page_id.space(), page_id.page_no());
		nvdimm_ipl_add_split_merge_map(page_id);
		return false;
	}

	//step3. Make log header
	// To recovery the page, we need (type, ptr, end_ptr)
	// look up the log0recv.cc: recv_parse_or_apply_log_rec_body()
	// page별로, sequential하게 log 저장 (hdr, body) : (hdr, body) ...
	IPL_LOG_HDR log_hdr;
  	log_hdr.body_len = len;
  	log_hdr.type = type;


 	 //IPL header 저장.
	memcpy(nvdimm_ptr + ipl_start_offset + ipl_write_pointer, &log_hdr, sizeof(IPL_LOG_HDR));
	flush_cache(nvdimm_ptr + ipl_start_offset + ipl_write_pointer, sizeof(IPL_LOG_HDR));
  	ipl_write_pointer += sizeof(IPL_LOG_HDR);

  	//IPL body 저장.
	memcpy(nvdimm_ptr + ipl_start_offset + ipl_write_pointer, log, len);
	flush_cache(nvdimm_ptr + ipl_start_offset + ipl_write_pointer , len);
	ipl_write_pointer += len;

	//IPL write pointer 업데이트
	it = ipl_map.find(page_id);
	it->second.ipl_write_pointer = ipl_write_pointer;

	// fprintf(stderr, "[NVDIMM_ADD_LOG]: Add log complete: (%u, %u), type: %u, len: %lu start_offset %lu wp:%lu\n", page_id.space(), page_id.page_no(), log_hdr.type, log_hdr.body_len, ipl_map[page_id], ipl_wp[page_id] );
  	mtr_commit(&temp_mtr);
	return true;
}

void nvdimm_ipl_log_apply(page_id_t page_id, buf_block_t* block) {

	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);

	// step1. read current IPL log using page_id

	map<page_id_t, ipl_info>::iterator it = ipl_map.find(page_id);

	uint64_t ipl_start_offset = it->second.ipl_start_offset;
	uint64_t write_pointer = it->second.ipl_write_pointer;

	byte * start_ptr = nvdimm_ptr + ipl_start_offset; 
	byte * end_ptr = nvdimm_ptr + ipl_start_offset + write_pointer; // log

	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying start!";
	
  	while (start_ptr < end_ptr) {

		// log_hdr를 가져와서 저장
		IPL_LOG_HDR log_hdr;
		memcpy(&log_hdr, start_ptr, sizeof(IPL_LOG_HDR));
		flush_cache(&log_hdr, sizeof(IPL_LOG_HDR));
		start_ptr += sizeof(IPL_LOG_HDR);
		// fprintf(stderr, "(%u, %u) Type : %d len: %lu\n",page_id.space(), page_id.page_no(), log_hdr.type, log_hdr.body_len);

		//log apply 진행 후, recovery 시작 위치 이동.
		recv_parse_or_apply_log_rec_body(log_hdr.type, start_ptr, start_ptr + log_hdr.body_len, page_id.space(), page_id.page_no(), block, &temp_mtr);
		start_ptr += log_hdr.body_len;
	}
  	temp_mtr.discard_modifications();
	mtr_commit(&temp_mtr);
	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying finish!";
	return;
}


void nvdimm_ipl_erase(page_id_t page_id) { // 굳이 page가 들어갈 필요는 없음.
	// When the page is flushed, we need to delete IPL log from NVDIMM
	// step1. read current IPL log using page_id
	map<page_id_t, ipl_info>::iterator it = ipl_map.find(page_id);
	uint64_t ipl_start_offset = it->second.ipl_start_offset;
	
	// step2. delete IPL Logs 
	unsigned char* ptr = nvdimm_ptr + ipl_start_offset;
	// ib::info() << page_id.space() << ":" << page_id.page_no()  << " IPL delete start!";
	memset(ptr, 0x00, IPL_LOG_REGION_SZ);
	flush_cache(ptr, IPL_LOG_REGION_SZ);
	it->second.ipl_write_pointer = 0;
	// ib::info() << page_id.space() << ":" << page_id.page_no()  << " IPL delete finish!";

	return;
}

bool nvdimm_ipl_lookup(page_id_t page_id) {
	// return true, 
	// if page exists in IPL region and IPL log is written
	map<page_id_t, ipl_info>::iterator it = ipl_map.find(page_id);
	if(it == ipl_map.end() || it->second.ipl_write_pointer == 0){
		return false;
	}
	return true;
}

void nvdimm_ipl_add_split_merge_map(page_id_t page_id){
	//Function to page is splited or merge
	// ib::info() << "(" <<page_id.space() << ", " << page_id.page_no() << ")" << " Add split_merge_map!";
	//is not in split merge map
	map<page_id_t, ipl_info>::iterator it = ipl_map.find(page_id);

	if(it != ipl_map.end()){
		it->second.have_to_flush = true;
	}
	
}

void nvdimm_ipl_remove_split_merge_map(page_id_t page_id){
	//Function to page is splited or merge
	if(nvdimm_ipl_lookup(page_id)){
		nvdimm_ipl_erase(page_id);
	}
}

bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id){
	map<page_id_t, ipl_info>::iterator it = ipl_map.find(page_id);

	if(it == ipl_map.end()) return false;
	return it->second.have_to_flush; 
}


