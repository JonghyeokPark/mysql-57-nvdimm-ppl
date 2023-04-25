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


void alloc_new_ipl_info(page_id_t page_id){
	//Check nvdimm region is full.
	ipl_info * new_ipl_info = static_cast<ipl_info *>(ut_zalloc_nokey(sizeof(ipl_info)));
	new_ipl_info->ipl_start_offset = nvdimm_info->nvdimm_offset;
	new_ipl_info->ipl_write_pointer = 0;
	new_ipl_info->have_to_flush = false;
	mutex_create(LATCH_ID_IPL_PER_PAGE, &new_ipl_info->ipl_per_page_mutex);
	mutex_enter(&new_ipl_info->ipl_per_page_mutex);
	ipl_map.insert(std::make_pair(page_id.fold(), new_ipl_info));
	mutex_exit(&nvdimm_info->ipl_map_mutex);
}


bool nvdimm_ipl_add(const page_id_t page_id, unsigned char *log, ulint len, mlog_id_t type) {
	// step1. get offset in NVDIMM IPL region from ipl_map table
	mtr_t temp_mtr;
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it;
	uint64_t ipl_start_offset = 0;
	ulint ipl_write_pointer = 0;
	IPL_LOG_HDR log_hdr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	it = ipl_map.find(page_id.fold());
	//if first write, alloc the new ipl info
	// fprintf(stderr, "buffer size: %zu\n", max_log_size);
	if (it == ipl_map.end()) {
		mutex_enter(&nvdimm_info->nvdimm_offset_mutex);
		alloc_new_ipl_info(page_id);
		ipl_start_offset = nvdimm_info->nvdimm_offset;
		ipl_write_pointer = 0;
		if(nvdimm_info->nvdimm_offset + 4096 >= max_log_size){
			fprintf(stderr, "Overflow!2\n");
			mutex_exit(&nvdimm_info->nvdimm_offset_mutex);
			mutex_exit(&it->second->ipl_per_page_mutex);
			return false;
		}
		nvdimm_info->nvdimm_offset += IPL_LOG_REGION_SZ;
		mutex_exit(&nvdimm_info->nvdimm_offset_mutex);
	}
	else{
		mutex_enter(&it->second->ipl_per_page_mutex);
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		ipl_start_offset = it->second->ipl_start_offset;
		ipl_write_pointer = it->second->ipl_write_pointer;
	}

	// step2. check capacity 
	if (ipl_write_pointer + len + sizeof(IPL_LOG_HDR) >= IPL_LOG_REGION_SZ) {
    	// fprintf(stderr, "Cannot Save the log, Add the Flush (%u, %u)\n", page_id.space(), page_id.page_no());
		it->second->have_to_flush = true;
		mutex_exit(&it->second->ipl_per_page_mutex);
		return false;
	}

	//step3. Make log header
	// To recovery the page, we need (type, ptr, end_ptr)
	// look up the log0recv.cc: recv_parse_or_apply_log_rec_body()
	// page별로, sequential하게 log 저장 (hdr, body) : (hdr, body) ...
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
	it = ipl_map.find(page_id.fold());
	it->second->ipl_write_pointer = ipl_write_pointer;

	// fprintf(stderr, "[NVDIMM_ADD_LOG]: Add log complete: (%u, %u), type: %u, len: %lu start_offset %lu wp:%lu\n", page_id.space(), page_id.page_no(), log_hdr.type, log_hdr.body_len, ipl_start_offset, ipl_write_pointer);
	mutex_exit(&it->second->ipl_per_page_mutex);
  	mtr_commit(&temp_mtr);
	return true;
}

void nvdimm_ipl_log_apply(page_id_t page_id, buf_block_t* block) {
	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying start!";

	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	buf_page_mutex_enter(block);

	// step1. read current IPL log using page_id
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info *>::iterator it = ipl_map.find(page_id.fold());
	mutex_enter(&it->second->ipl_per_page_mutex);
	mutex_exit(&nvdimm_info->ipl_map_mutex);

	uint64_t ipl_start_offset = it->second->ipl_start_offset;
	ulint write_pointer = it->second->ipl_write_pointer;
	byte * start_ptr = nvdimm_ptr + ipl_start_offset; 
	byte * end_ptr = nvdimm_ptr + ipl_start_offset + write_pointer; // log
	mutex_exit(&it->second->ipl_per_page_mutex);

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
	fprintf(stderr,"[apply]ipl page: (%u, %u) oldest: %zu, newest: %zu\n", block->page.id.space(), block->page.id.page_no(), block->page.oldest_modification, block->page.newest_modification);
  	temp_mtr.discard_modifications();
	
	buf_page_mutex_exit(block);
	mtr_commit(&temp_mtr);
	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying finish!";
	return;
}


bool nvdimm_ipl_lookup(page_id_t page_id) {
	// return true, 
	// if page exists in IPL region and IPL log is written
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(page_id.fold());
	if(it == ipl_map.end()){
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		return false;
	}
	else {
		mutex_enter(&it->second->ipl_per_page_mutex);
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		if(it->second->ipl_write_pointer == 0){
			mutex_exit(&it->second->ipl_per_page_mutex);
			return false;
		}
	}
	mutex_exit(&it->second->ipl_per_page_mutex);
	return true;
}

void nvdimm_ipl_add_split_merge_map(page_id_t page_id){
	// fprintf(stderr, "ipl_add page(%u, %u)\n", page_id.space(), page_id.page_no());
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(page_id.fold());
	if(it != ipl_map.end()){
		
		mutex_enter(&it->second->ipl_per_page_mutex);
		it->second->have_to_flush = true;
		mutex_exit(&it->second->ipl_per_page_mutex);
		mutex_exit(&nvdimm_info->ipl_map_mutex);	
	}
	else{
		mutex_enter(&nvdimm_info->nvdimm_offset_mutex);
		alloc_new_ipl_info(page_id);
		it = ipl_map.find(page_id.fold());
		it->second->have_to_flush = true;
		if(nvdimm_info->nvdimm_offset + 4096 >= max_log_size){
			fprintf(stderr, "Overflow!2\n");
			mutex_exit(&nvdimm_info->nvdimm_offset_mutex);
			mutex_exit(&it->second->ipl_per_page_mutex);
			return;
		}
		nvdimm_info->nvdimm_offset += IPL_LOG_REGION_SZ;
		mutex_exit(&nvdimm_info->nvdimm_offset_mutex);
		mutex_exit(&it->second->ipl_per_page_mutex);
	}
	
	
}

bool nvdimm_ipl_remove_split_merge_map(page_id_t page_id){
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(page_id.fold());
	if(it != ipl_map.end()){
		mutex_enter(&it->second->ipl_per_page_mutex);
		unsigned char* ptr = nvdimm_ptr + it->second->ipl_start_offset;
		memset(ptr, 0x00, IPL_LOG_REGION_SZ);
		flush_cache(ptr, IPL_LOG_REGION_SZ);
		it->second->ipl_write_pointer = 0;
		it->second->have_to_flush=false;
		mutex_exit(&it->second->ipl_per_page_mutex);
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		return true;
	}
	else{
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		return true;
	}
	
}

bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id){
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(page_id.fold());
	if(it == ipl_map.end()) {
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		return false;
	}
	mutex_enter(&it->second->ipl_per_page_mutex);
	mutex_exit(&nvdimm_info->ipl_map_mutex);
	bool return_value = it->second->have_to_flush;
	mutex_exit(&it->second->ipl_per_page_mutex);
	return return_value;
}


