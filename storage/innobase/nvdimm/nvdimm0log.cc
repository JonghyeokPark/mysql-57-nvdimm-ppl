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
//싹다 클래스로 캡슐레이션을 해버려야 되나...
// 나중에 시간되면 해보기.



unsigned char * get_static_ipl_address(page_id_t page_id){
	unsigned char * static_ipl_address = nvdimm_info->static_start_pointer + (nvdimm_info->static_ipl_count++ * nvdimm_info->static_ipl_per_page_size);
	mach_write_to_4(static_ipl_address, page_id.space());
	static_ipl_address += 4;
	mach_write_to_4(static_ipl_address, page_id.page_no());
	static_ipl_address += 4;
	mach_write_to_8(static_ipl_address, 0UL);
	static_ipl_address += 8;
	return static_ipl_address;
}

bool alloc_dynamic_ipl_region(std::tr1::unordered_map<ulint, ipl_info * >::iterator it){
	if(it->second->page_ipl_region_size >= 1024 + 256){ // 일단, 1KB + 256Byte까지만 할당 가능하게 하기.
		fprintf(stderr, "Max page_ipl_region_size\n");
		mutex_exit(&nvdimm_info->dynamic_region_mutex);
		return false;
	}
	if(check_static_or_dynamic_ipl_region_full(false)){
		fprintf(stderr, "Dynamic_ipl_region Overflow!\n");
		mutex_exit(&nvdimm_info->dynamic_region_mutex);
		return false;
	}
	unsigned char * dynamic_address = nvdimm_info->dynamic_start_pointer + (nvdimm_info->dynamic_ipl_count++ * nvdimm_info->dynamic_ipl_per_page_size);
	unsigned char * pointer_to_store_dynamic_address = it->second->static_region_pointer + DYNAMIC_ADDRESS_OFFSET;
	mach_write_to_8(pointer_to_store_dynamic_address, (uint64_t)dynamic_address);
	flush_cache(pointer_to_store_dynamic_address, 8);
	mutex_exit(&nvdimm_info->dynamic_region_mutex);
	it->second->dynamic_region_pointer = dynamic_address;
	fprintf(stderr, "Dynamic region allocated %p\n", dynamic_address);
	return true;
}

bool alloc_static_ipl_info_and_region(page_id_t page_id){
	//Check nvdimm region is full.
	
	ipl_info * new_ipl_info = static_cast<ipl_info *>(ut_zalloc_nokey(sizeof(ipl_info)));
	fprintf(stderr, "alloc_static_ipl_info_and_region page_id: (%u, %u)\n", page_id.space(), page_id.page_no());
	new_ipl_info->static_region_pointer = get_static_ipl_address(page_id);
	new_ipl_info->dynamic_region_pointer = NULL;
	new_ipl_info->page_ipl_region_size = IPL_LOG_HEADER_SIZE;

	mutex_create(LATCH_ID_IPL_PER_PAGE, &new_ipl_info->ipl_per_page_mutex);
	mutex_enter(&new_ipl_info->ipl_per_page_mutex);
	ipl_map.insert(std::make_pair(page_id.fold(), new_ipl_info));
	mutex_exit(&nvdimm_info->ipl_map_mutex);
	return true;
}

bool check_static_or_dynamic_ipl_region_full(bool is_static_page){
	bool return_value = is_static_page ? nvdimm_info->static_ipl_count >= nvdimm_info->static_ipl_max_page_count : nvdimm_info->dynamic_ipl_count >= nvdimm_info->dynamic_ipl_max_page_count;
	return return_value;
}


ulint write_to_static_region(std::tr1::unordered_map<ulint, ipl_info * >::iterator it, ulint len, unsigned char * write_ipl_log_buffer){
	unsigned char * write_pointer = it->second->static_region_pointer + it->second->page_ipl_region_size;
	
	//static 영역에 자리가 없는 경우.
	if(nvdimm_info->static_ipl_per_page_size <= it->second->page_ipl_region_size){
		return len;
	}
	ulint can_write_size = nvdimm_info->static_ipl_per_page_size - it->second->page_ipl_region_size;
	//static 영역에 다 못 쓰는 경우,
	if(can_write_size < len){ 
		memcpy(write_pointer, write_ipl_log_buffer, can_write_size);
		flush_cache(write_pointer, can_write_size);
		it->second->page_ipl_region_size += can_write_size;
		return len - can_write_size;
	}
	//static 영역에 다 쓸 수 있는 경우.
	memcpy(write_pointer, write_ipl_log_buffer, len);
	flush_cache(write_pointer, len);
	it->second->page_ipl_region_size += len;

	mlog_id_t log_type = mlog_id_t(mach_read_from_1(write_pointer));
	write_pointer += 1;
	ulint body_len = mach_read_from_2(write_pointer);
	write_pointer += 2;
	fprintf(stderr, "Save complete, Read log! write_pointer: %p type: %d, len: %u\n",write_pointer, log_type, body_len);

	return 0;
	
}

ulint write_to_dynamic_region(std::tr1::unordered_map<ulint, ipl_info * >::iterator it, ulint len, unsigned char * write_ipl_log_buffer){
	ulint dynamic_page_size = it->second->page_ipl_region_size - nvdimm_info->static_ipl_per_page_size;
	unsigned char * write_pointer = it->second->dynamic_region_pointer + dynamic_page_size;
	fprintf(stderr, "page_ipl_region_size: %u, static_ipl_per_page_size: %u, dynamic_page_size %u\n", it->second->page_ipl_region_size, nvdimm_info->static_ipl_per_page_size, dynamic_page_size);
	
	//dynamic 영역에 자리가 없는 경우.
	if(nvdimm_info->dynamic_ipl_per_page_size <= dynamic_page_size){
		return len;
	}
	ulint can_write_size = nvdimm_info->dynamic_ipl_per_page_size - dynamic_page_size;
	//dynamic 영역에 다 못 쓰는 경우,
	if(can_write_size < len){ 
		memcpy(write_pointer, write_ipl_log_buffer, can_write_size);
		flush_cache(write_pointer, can_write_size);
		it->second->page_ipl_region_size += can_write_size;
		return len - can_write_size;
	}
	//dynamic 영역에 다 쓸 수 있는 경우.
	memcpy(write_pointer, write_ipl_log_buffer, len);
	flush_cache(write_pointer, len);
	it->second->page_ipl_region_size += len;

	mlog_id_t log_type = mlog_id_t(mach_read_from_1(write_pointer));
	ulint body_len = mach_read_from_2(write_pointer + 1);
	fprintf(stderr, "Save complete in Dynamic page, Read log! write_pointer: %p type: %d, len: %u\n",write_pointer, log_type, body_len);
	return 0;
}


bool write_ipl_log_header_and_body(std::tr1::unordered_map<ulint, ipl_info * >::iterator it, ulint len, mlog_id_t type, unsigned char * log, buf_page_t * bpage){
	//하나의 로그 header + body 만들기.
	unsigned char* write_ipl_log_buffer = (byte *)calloc(len + 3, sizeof(char));
	ulint offset = 0;
	unsigned char store_type = type;
	unsigned short store_len = len;
	mach_write_to_1(write_ipl_log_buffer + offset, store_type);
	offset += 1;
	mach_write_to_2(write_ipl_log_buffer + offset, store_len);
	offset += 2;
	memcpy(write_ipl_log_buffer + offset, log, len);

	// fprintf(stderr, "Write log! (%u, %u) Type : %d len: %lu\n",page_id.space(), page_id.page_no(), store_type, store_len);
	ulint have_to_write_len = len + 3;
	ulint remain_len = write_to_static_region(it, have_to_write_len, write_ipl_log_buffer);
	
	//한 페이지당 사용할 static 영역이 다 찬 경우.
	if(remain_len > 0){
		if(it->second->dynamic_region_pointer == NULL){
			if(!alloc_dynamic_ipl_region(it)){
				//Dynamic 영역을 할당 받을 수 없는 경우.
				free(write_ipl_log_buffer);
				mutex_exit(&it->second->ipl_per_page_mutex);
				return false;
			}
		}
		offset = have_to_write_len - remain_len; // 내가 로그를 적은 길이를 더해 줌.
		fprintf(stderr, "remain_len : %u, write dynamic region page_id(%u, %u)\n", remain_len, bpage->id.space(), bpage->id.page_no());
		remain_len = write_to_dynamic_region(it, remain_len, write_ipl_log_buffer + offset);
		//한 페이지당 사용할 dynamic 영역이 다 찬 경우.
		if(remain_len > 0){
			//나중에 수정하기####
			fprintf(stderr, "dynamic regions also full!! page_id:(%u, %u)\n", bpage->id.space(), bpage->id.page_no());
			bpage->is_split_page = true;
			free(write_ipl_log_buffer);
			mutex_exit(&it->second->ipl_per_page_mutex);
			return false;
		}
		
	}
	free(write_ipl_log_buffer);
	mutex_exit(&it->second->ipl_per_page_mutex);
	return true;
}


bool nvdimm_ipl_add(const page_id_t page_id, unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage) {
	// step1. get offset in NVDIMM IPL region from ipl_map table
	fprintf(stderr, "Add log start! (%u, %u) Type : %d len: %lu\n",page_id.space(), page_id.page_no(), type, len);
	mtr_t temp_mtr;
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	it = ipl_map.find(page_id.fold());
	//if first write, alloc the new ipl info
	// fprintf(stderr, "buffer size: %zu\n", max_log_size);
	if (it == ipl_map.end()) {
		mutex_enter(&nvdimm_info->static_region_mutex);
		if(alloc_static_ipl_info_and_region(page_id)){
			it = ipl_map.find(page_id.fold());
			bpage->is_iplized = true;
			bpage->page_ipl_info = it->second;
		}

		if(check_static_or_dynamic_ipl_region_full(true)){
			fprintf(stderr, "static ipl region Overflow! \n");
			mutex_exit(&nvdimm_info->static_region_mutex);
			mutex_exit(&it->second->ipl_per_page_mutex);
			return false;
		}
		mutex_exit(&nvdimm_info->static_region_mutex);
	}
	else{
		mutex_enter(&it->second->ipl_per_page_mutex);
		mutex_exit(&nvdimm_info->ipl_map_mutex);
	}

	bool return_value = true;
	if(!write_ipl_log_header_and_body(it, len, type, log, bpage)){
		return_value = false;
	}

	// fprintf(stderr, "[NVDIMM_ADD_LOG]: Add log complete: (%u, %u), type: %u, len: %lu start_offset %lu wp:%lu\n", page_id.space(), page_id.page_no(), log_hdr.type, log_hdr.body_len, static_region_pointer, page_ipl_region_size);
	mutex_exit(&it->second->ipl_per_page_mutex);
  	mtr_commit(&temp_mtr);
	return return_value;
}



bool copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr){
	//NVDIMM 내 redo log를 메모리로 카피
	fprintf(stderr, "apply_info! page_id:(%u, %u) static: %p dynamic: %p log_len: %zu\n", apply_info->space_id, apply_info->page_no, apply_info->static_start_pointer, apply_info->dynamic_start_pointer, apply_info->log_len);

	byte * apply_log_buffer = (byte *)calloc(apply_info->log_len, sizeof(char));
	if(apply_info->dynamic_start_pointer == NULL){
		memcpy(apply_log_buffer, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE, apply_info->log_len);
		flush_cache(apply_log_buffer, apply_info->log_len);
	}
	else{ //dynamic 영역이 존재하는 경우도 카피.
		ulint offset = 0;
		
		memcpy(apply_log_buffer + offset, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE ,nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		flush_cache(apply_log_buffer + offset, apply_info->log_len);
		offset += STATIC_MAX_SIZE;

		uint dynamic_region_size = apply_info->log_len - (nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		fprintf(stderr, "dynamic_region_size: %u\n", dynamic_region_size);
		memcpy(apply_log_buffer + offset, apply_info->dynamic_start_pointer,dynamic_region_size);
		flush_cache(apply_log_buffer + offset, dynamic_region_size);

	}
	fprintf(stderr, "apply_log_buffer: %p\n", apply_log_buffer);
	//Log apply 시작.
	ipl_log_apply(apply_log_buffer, apply_info, temp_mtr);
	
	free(apply_log_buffer);
	return true;
	
	
}

void ipl_log_apply(byte * apply_log_buffer, apply_log_info * apply_info, mtr_t * temp_mtr){
	byte * start_ptr = apply_log_buffer;
	byte * end_ptr = start_ptr + apply_info->log_len;
	while (start_ptr < end_ptr) {
		// log_hdr를 가져와서 저장
		mlog_id_t log_type = mlog_id_t(mach_read_from_1(start_ptr));
		start_ptr += 1;
		ulint body_len = mach_read_from_2(start_ptr);
		start_ptr += 2;
		fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu\n",apply_info->space_id, apply_info->page_no, log_type, body_len);

		//log apply 진행 후, recovery 시작 위치 이동.
		// recv_parse_or_apply_log_rec_body(log_type, start_ptr, start_ptr + body_len, apply_info->space_id, apply_info->page_no, apply_info->block, temp_mtr);
		start_ptr += body_len;
	}
	fprintf(stderr, "apply_log_buffer: %p\n", apply_log_buffer);
}

void nvdimm_ipl_log_apply(page_id_t page_id, buf_block_t* block) {
	buf_page_t * apply_page = (buf_page_t *)block;
	ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying start!";
	ib::info() << "have_to_flush : " <<apply_page->is_split_page << "is_iplized : " << apply_page->is_iplized ;
	fprintf(stderr, "static pointer : %p, dynamic pointer : %p\n", apply_page->page_ipl_info->static_region_pointer, apply_page->page_ipl_info->dynamic_region_pointer);
	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	buf_page_mutex_enter(block);

	// step1. read current IPL log using page_id
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info *>::iterator it = ipl_map.find(page_id.fold());
	mutex_enter(&it->second->ipl_per_page_mutex);
	mutex_exit(&nvdimm_info->ipl_map_mutex);


	apply_log_info apply_info;
	apply_info.static_start_pointer = it->second->static_region_pointer;
	apply_info.dynamic_start_pointer = it->second->dynamic_region_pointer;
	apply_info.log_len = it->second->page_ipl_region_size - IPL_LOG_HEADER_SIZE;
	apply_info.space_id = page_id.space();
	apply_info.page_no = page_id.page_no();
	apply_info.block = block;

	mutex_exit(&it->second->ipl_per_page_mutex);
	if(copy_log_to_mem_to_apply(&apply_info, &temp_mtr)){
		temp_mtr.discard_modifications();
		buf_page_mutex_exit(block);
		mtr_commit(&temp_mtr);
	}
  	
	ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying finish!";
	return;
}


// bool nvdimm_ipl_lookup(page_id_t page_id) {
// 	// return true, 
// 	// if page exists in IPL region and IPL log is written
// 	mutex_enter(&nvdimm_info->ipl_map_mutex);
// 	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(page_id.fold());
// 	if(it == ipl_map.end()){
// 		mutex_exit(&nvdimm_info->ipl_map_mutex);
// 		return false;
// 	}
// 	else {
// 		mutex_enter(&it->second->ipl_per_page_mutex);
// 		mutex_exit(&nvdimm_info->ipl_map_mutex);
// 		if(it->second->page_ipl_region_size - 16 == 0){
// 			mutex_exit(&it->second->ipl_per_page_mutex);
// 			return false;
// 		}
// 	}
// 	mutex_exit(&it->second->ipl_per_page_mutex);
// 	return true;
// }

void nvdimm_ipl_add_split_merge_map(page_id_t page_id){
	// fprintf(stderr, "ipl_add page(%u, %u)\n", page_id.space(), page_id.page_no());
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	buf_page_t * buf_page = buf_page_hash_get(buf_pool, page_id);
	buf_page->is_split_page = true;
}

bool nvdimm_ipl_remove_split_merge_map(page_id_t page_id){
	//flush 되는 애들이면, static 영역 반납 및 dynamic 영역 반납 수행.
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(page_id.fold());
	if(it != ipl_map.end()){
		fprintf(stderr, "IPL page Erase! page_id(%u, %u)\n", page_id.space(), page_id.page_no());
		mutex_enter(&it->second->ipl_per_page_mutex);
		unsigned char* ptr = it->second->static_region_pointer;
		if(ptr == NULL){
			mutex_exit(&it->second->ipl_per_page_mutex);
			mutex_exit(&nvdimm_info->ipl_map_mutex);
			return true;
		}
		memset(ptr, 0x00, nvdimm_info->static_ipl_per_page_size);
		flush_cache(ptr, nvdimm_info->static_ipl_per_page_size);
		it->second->dynamic_region_pointer = NULL;
		it->second->page_ipl_region_size = 16;
		mutex_exit(&it->second->ipl_per_page_mutex);
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		return true;
	}
	else{
		fprintf(stderr, "Not ipl page Erase page_id(%u, %u)\n", page_id.space(), page_id.page_no());
		mutex_exit(&nvdimm_info->ipl_map_mutex);
		return true;
	}
	
}

bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id){
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	buf_page_t * buf_page = buf_page_hash_get(buf_pool, page_id);
	return buf_page->is_split_page;
}


void set_for_ipl_page(buf_page_t* bpage){
	fprintf(stderr, "Read ipl page! page_id(%u, %u)\n", bpage->id.space(), bpage->id.page_no());
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<ulint, ipl_info * >::iterator it = ipl_map.find(bpage->id.fold());
	if(it != ipl_map.end()){
		bpage->is_iplized = true;
		bpage->is_split_page = false;
		bpage->page_ipl_info = it->second;
	}
	else{
		bpage->is_iplized = false;
		bpage->is_split_page = false;
		bpage->page_ipl_info = NULL;
	}
	mutex_exit(&nvdimm_info->ipl_map_mutex);
}