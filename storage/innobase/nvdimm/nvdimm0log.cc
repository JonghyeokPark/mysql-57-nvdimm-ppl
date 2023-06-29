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
	unsigned char * static_ipl_address = alloc_static_address_from_indirection_queue();
	if(static_ipl_address == NULL)	return NULL; // 더이상 할당할 static이 없는 경우
	ulint offset = 0;
	mach_write_to_4(static_ipl_address + offset, page_id.space());
	offset += 4;
	mach_write_to_4(static_ipl_address + offset, page_id.page_no());
	offset += 4;
	mach_write_to_4(static_ipl_address + offset, 0UL);
	offset += 4;
	return static_ipl_address;
}

bool alloc_dynamic_ipl_region(ipl_info * page_ipl_info){
	unsigned char * dynamic_address = alloc_dynamic_address_from_indirection_queue();
	if(dynamic_address == NULL)	return false; 
	unsigned char * pointer_to_store_dynamic_address = page_ipl_info->static_region_pointer + DYNAMIC_ADDRESS_OFFSET;
	mach_write_to_4(pointer_to_store_dynamic_address, get_ipl_index_from_addr(nvdimm_info->dynamic_start_pointer, dynamic_address, nvdimm_info->dynamic_ipl_per_page_size));
	flush_cache(pointer_to_store_dynamic_address, 4);
	page_ipl_info->dynamic_region_pointer = dynamic_address;
	// fprintf(stderr, "Dynamic region allocated %p\n", dynamic_address);
	return true;
}

ipl_info * alloc_static_ipl_info_and_region(page_id_t page_id){
	//Check nvdimm region is full.
	unsigned char * static_ipl_address = get_static_ipl_address(page_id);
	if(static_ipl_address == NULL)	return NULL;
	ipl_info * new_ipl_info = static_cast<ipl_info *>(ut_zalloc_nokey(sizeof(ipl_info)));
	new_ipl_info->static_region_pointer = static_ipl_address;
	new_ipl_info->dynamic_region_pointer = NULL;
	new_ipl_info->page_ipl_region_size = IPL_LOG_HEADER_SIZE;
	// fprintf(stderr, "alloc_static_ipl_info_and_region page_id: (%u, %u), static_ipl: %p\n", page_id.space(), page_id.page_no(), new_ipl_info->static_region_pointer);
	return new_ipl_info;
}


ulint write_to_static_region(ipl_info * page_ipl_info, ulint len, unsigned char * write_ipl_log_buffer){
	unsigned char * write_pointer = page_ipl_info->static_region_pointer + page_ipl_info->page_ipl_region_size;
	
	//static 영역에 자리가 없는 경우.
	if(nvdimm_info->static_ipl_per_page_size <= page_ipl_info->page_ipl_region_size){
		return len;
	}
	ulint can_write_size = nvdimm_info->static_ipl_per_page_size - page_ipl_info->page_ipl_region_size;
	//static 영역에 다 못 쓰는 경우,
	if(can_write_size < len){ 
		memcpy(write_pointer, write_ipl_log_buffer, can_write_size);
		flush_cache(write_pointer, can_write_size);
		page_ipl_info->page_ipl_region_size += can_write_size;
		return len - can_write_size;
	}
	//static 영역에 다 쓸 수 있는 경우.
	memcpy(write_pointer, write_ipl_log_buffer, len);
	flush_cache(write_pointer, len);
	page_ipl_info->page_ipl_region_size += len;

	mlog_id_t log_type = mlog_id_t(mach_read_from_1(write_pointer));
	write_pointer += 1;
	ulint body_len = mach_read_from_2(write_pointer);
	write_pointer += 2;
	// fprintf(stderr, "Save complete, Read log! write_pointer: %p type: %d, len: %u\n",write_pointer, log_type, body_len);

	return 0;
	
}

ulint write_to_dynamic_region(ipl_info * page_ipl_info, ulint len, unsigned char * write_ipl_log_buffer){
	ulint dynamic_page_size = page_ipl_info->page_ipl_region_size - nvdimm_info->static_ipl_per_page_size;
	unsigned char * write_pointer = page_ipl_info->dynamic_region_pointer + dynamic_page_size;
	// fprintf(stderr, "page_ipl_region_size: %u, static_ipl_per_page_size: %u, dynamic_page_size %u\n", page_ipl_info->page_ipl_region_size, nvdimm_info->static_ipl_per_page_size, dynamic_page_size);
	
	//dynamic 영역에 자리가 없는 경우.
	if(nvdimm_info->dynamic_ipl_per_page_size <= dynamic_page_size){
		return len;
	}
	ulint can_write_size = nvdimm_info->dynamic_ipl_per_page_size - dynamic_page_size;
	//dynamic 영역에 다 못 쓰는 경우,
	if(can_write_size < len){ 
		memcpy(write_pointer, write_ipl_log_buffer, can_write_size);
		flush_cache(write_pointer, can_write_size);
		page_ipl_info->page_ipl_region_size += can_write_size;
		return len - can_write_size;
	}
	//dynamic 영역에 다 쓸 수 있는 경우.
	memcpy(write_pointer, write_ipl_log_buffer, len);
	flush_cache(write_pointer, len);
	page_ipl_info->page_ipl_region_size += len;

	mlog_id_t log_type = mlog_id_t(mach_read_from_1(write_pointer));
	ulint body_len = mach_read_from_2(write_pointer + 1);
	// fprintf(stderr, "Save complete in Dynamic page, Read log! write_pointer: %p type: %d, len: %u\n",write_pointer, log_type, body_len);
	return 0;
}


bool write_ipl_log_header_and_body(buf_page_t * bpage, ulint len, mlog_id_t type, unsigned char * log){
	//하나의 로그 header + body 만들기.
	ipl_info * page_ipl_info = bpage->page_ipl_info;
	page_id_t page_id = bpage->id;
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
	ulint remain_len = write_to_static_region(page_ipl_info, have_to_write_len, write_ipl_log_buffer);
	
	//한 페이지당 사용할 static 영역이 다 찬 경우.
	if(remain_len > 0){
		if(page_ipl_info->dynamic_region_pointer == NULL){
			if(!alloc_dynamic_ipl_region(page_ipl_info)){
				//Dynamic 영역을 할당 받을 수 없는 경우, page를 flush하고 새로 할당받기
				bpage->is_split_page = true;
				free(write_ipl_log_buffer);
				return false;
			}
		}
		offset = have_to_write_len - remain_len; // 내가 로그를 적은 길이를 더해 줌.
		// fprintf(stderr, "remain_len : %u, write dynamic region page_id(%u, %u)\n", remain_len, page_id.space(), page_id.page_no());
		remain_len = write_to_dynamic_region(page_ipl_info, remain_len, write_ipl_log_buffer + offset);
		//한 페이지당 사용할 dynamic 영역이 다 찬 경우.
		if(remain_len > 0){
			//나중에 수정하기####
			// fprintf(stderr, "dynamic regions also full!! page_id:(%u, %u)\n", page_id.space(), page_id.page_no());
			bpage->is_split_page = true;
			free(write_ipl_log_buffer);
			return false;
		}
		
	}
	free(write_ipl_log_buffer);
	return true;
}


bool nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage){
	page_id_t page_id = bpage->id;
	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	// fprintf(stderr, "Add log start! (%u, %u) Type : %d len: %lu\n",page_id.space(), page_id.page_no(), type, len);
	//if first write, alloc the new ipl info
	// fprintf(stderr, "buffer size: %zu\n", max_log_size);
	if (!bpage->is_iplized) {
		bpage->page_ipl_info = alloc_static_ipl_info_and_region(page_id);
		if(bpage->page_ipl_info == NULL){
			bpage->is_split_page = true;
			mtr_commit(&temp_mtr);
			return false;
		}
		bpage->is_iplized = true;
	}

	bool return_value = true;
	if(!write_ipl_log_header_and_body(bpage, len, type, log)){
		return_value = false;
	}
		if(return_value){
		// fprintf(stderr, "log add, Page id : (%u, %u) Type : %d len: %lu, static pointer : %p, dynamic pointer : %p ipl_len : %u\n",page_id.space(), page_id.page_no(), type, len, bpage->page_ipl_info->static_region_pointer, bpage->page_ipl_info->dynamic_region_pointer, bpage->page_ipl_info->page_ipl_region_size);
	}
	
  	mtr_commit(&temp_mtr);
	return return_value;
}



bool copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr){
	//NVDIMM 내 redo log를 메모리로 카피
	// fprintf(stderr, "apply_info! page_id:(%u, %u) static: %p dynamic: %p log_len: %zu\n", apply_info->space_id, apply_info->page_no, apply_info->static_start_pointer, apply_info->dynamic_start_pointer, apply_info->log_len);

	byte * apply_log_buffer = (byte *)calloc(apply_info->log_len, sizeof(char));
	if(apply_info->dynamic_start_pointer == NULL){
		memcpy(apply_log_buffer, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE, apply_info->log_len);
		flush_cache(apply_log_buffer, apply_info->log_len);
	}
	else{ //dynamic 영역이 존재하는 경우도 카피.
		ulint offset = 0;
		
		memcpy(apply_log_buffer + offset, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE ,nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		flush_cache(apply_log_buffer + offset, apply_info->log_len);
		offset += nvdimm_info->static_ipl_per_page_size	- IPL_LOG_HEADER_SIZE;

		uint dynamic_region_size = apply_info->log_len - (nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		// fprintf(stderr, "dynamic_region_size: %u\n", dynamic_region_size);
		memcpy(apply_log_buffer + offset, apply_info->dynamic_start_pointer,dynamic_region_size);
		flush_cache(apply_log_buffer + offset, dynamic_region_size);

	}
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
		// fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu\n",apply_info->space_id, apply_info->page_no, log_type, body_len);

		//log apply 진행 후, recovery 시작 위치 이동.
		recv_parse_or_apply_log_rec_body(log_type, start_ptr, start_ptr + body_len, apply_info->space_id, apply_info->page_no, apply_info->block, temp_mtr);
		start_ptr += body_len;
	}
	temp_mtr->discard_modifications();
	mtr_commit(temp_mtr);
}

void nvdimm_ipl_log_apply(buf_block_t* block) {
	buf_page_t * apply_page = (buf_page_t *)block;
	ipl_info * page_ipl_info = apply_page->page_ipl_info;
	page_id_t page_id = apply_page->id;

	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);

	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying start!";
	// ib::info() << "have_to_flush : " <<apply_page->is_split_page << "is_iplized : " << apply_page->is_iplized ;
	// fprintf(stderr, "static pointer : %p, dynamic pointer : %p\n", apply_page->page_ipl_info->static_region_pointer, apply_page->page_ipl_info->dynamic_region_pointer);

	// step1. read current IPL log using page_id

	apply_log_info apply_info;
	apply_info.static_start_pointer = page_ipl_info->static_region_pointer;
	apply_info.dynamic_start_pointer = get_addr_from_ipl_index(nvdimm_info->dynamic_start_pointer, mach_read_from_4(page_ipl_info->static_region_pointer + DYNAMIC_ADDRESS_OFFSET), nvdimm_info->dynamic_ipl_per_page_size);
	apply_info.log_len = page_ipl_info->page_ipl_region_size - IPL_LOG_HEADER_SIZE;
	apply_info.space_id = page_id.space();
	apply_info.page_no = page_id.page_no();
	apply_info.block = block;

	if(copy_log_to_mem_to_apply(&apply_info, &temp_mtr)){
		// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying finish!";
	}

	return;
}


void insert_page_ipl_info_in_hash_table(buf_page_t * bpage){
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	// fprintf(stderr, "Save ipl info page(%u, %u), static: %p, dynamic: %p\n", bpage->id.space(), bpage->id.page_no(), bpage->page_ipl_info->static_region_pointer, bpage->page_ipl_info->dynamic_region_pointer);
	page_id_t page_id = bpage->id;
	std::tr1::unordered_map<page_id_t, ipl_info * >::iterator it = ipl_map.find(bpage->id);
	if(it == ipl_map.end()){
		ipl_map.insert(std::make_pair(bpage->id, bpage->page_ipl_info));
	}
	mutex_exit(&nvdimm_info->ipl_map_mutex);
}

void nvdimm_ipl_add_split_merge_map(page_id_t page_id){
	// fprintf(stderr, "ipl_add Split page(%u, %u)\n", page_id.space(), page_id.page_no());
	//buf_page_hash_get_s_locked로 시도해보기.
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	buf_page_t * buf_page = buf_page_get_also_watch(buf_pool, page_id);
	buf_page->is_split_page = true;
}

bool nvdimm_ipl_remove_split_merge_map(buf_page_t * bpage, page_id_t page_id){
	// fprintf(stderr, "ipl_remove page(%u, %u), static: %p, dynamic: %p, page_ipl_info : %p\n", page_id.space(), page_id.page_no(), bpage->page_ipl_info->static_region_pointer, bpage->page_ipl_info->dynamic_region_pointer, bpage->page_ipl_info);
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	ipl_info * page_ipl_info = bpage->page_ipl_info;
	bpage->is_iplized = false;
	bpage->is_split_page = false;
	ipl_map.erase(page_id);
	if(free_dynamic_address_to_indirection_queue(page_ipl_info->dynamic_region_pointer) && free_static_address_to_indirection_queue(page_ipl_info->static_region_pointer)){
		ut_free(bpage->page_ipl_info);
		bpage->page_ipl_info = NULL; // 확인해보기.
	}
	mutex_exit(&nvdimm_info->ipl_map_mutex);
	return true;
}

bool nvdimm_ipl_is_split_or_merge_page(page_id_t page_id){
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	buf_page_t * buf_page = buf_page_get_also_watch(buf_pool, page_id);
	return buf_page->is_split_page;
}


void set_for_ipl_page(buf_page_t* bpage){
	mutex_enter(&nvdimm_info->ipl_map_mutex);
	std::tr1::unordered_map<page_id_t, ipl_info * >::iterator it = ipl_map.find(bpage->id);
	if(it != ipl_map.end()){
		// fprintf(stderr, "Read ipl page! page_id(%u, %u)\n", bpage->id.space(), bpage->id.page_no());
		bpage->is_iplized = true;
		bpage->is_split_page = false;
		bpage->page_ipl_info = it->second;
	}
	else{
		bpage->is_iplized = false;
		bpage->is_split_page = false;
		bpage->page_ipl_info = NULL;
	}
	bpage->is_dirtified = false;
	mutex_exit(&nvdimm_info->ipl_map_mutex);
}

void print_page_info(buf_page_t * bpage){
	ipl_info * page_ipl_info = bpage->page_ipl_info;
	if(page_ipl_info != NULL){
		// fprintf(stderr, "buf_do_flush_list_batch IPLIZED: page: (%u, %u) static_pointer : %p, dynamic_pointer : %p, is_iplized : %u, oldest_modification : %zu, newest_modification : %zu\n", 
		// bpage->id.space(), bpage->id.page_no(), page_ipl_info->static_region_pointer, page_ipl_info->dynamic_region_pointer, bpage->is_iplized, bpage->oldest_modification, bpage->newest_modification);
	}
	else{
		// fprintf(stderr, "Normal buf_do_flush_list_batch  : page: (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
	}
	
}

//Dynamic 영역을 가지고 있는 checkpoint page인지 확인하기.
bool check_not_flush_page(buf_page_t * bpage, buf_flush_t flush_type){
	if(bpage->is_iplized == false){
		// fprintf(stderr, "[FLUSH] Normal page: (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
		return false;
	}
	else{
		ipl_info * page_ipl_info = bpage->page_ipl_info;
		if(bpage->is_split_page){
			if(nvdimm_ipl_remove_split_merge_map(bpage, bpage->id)){
				// fprintf(stderr, "[FLUSH]split ipl page: (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
				return false;
			}
		}
		if(page_ipl_info->dynamic_region_pointer == NULL){
			// fprintf(stderr, "[Not Flush]Only Static ipl page: (%u, %u) flush_type %u\n", bpage->id.space(), bpage->id.page_no(), flush_type);
			return true;
		}
		else{
			if(flush_type == BUF_FLUSH_LIST){
				// fprintf(stderr, "[Not Flush]Checkpoint ipl page: (%u, %u) flush_type %u\n", bpage->id.space(), bpage->id.page_no(), flush_type);
				return true;
			}
			else{
				if(nvdimm_ipl_remove_split_merge_map(bpage, bpage->id)){
					// fprintf(stderr, "[FLUSH]Dynamic ipl page: (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
					return false;
				}
				
			}

		}

	}
	// fprintf(stderr, "[FLUSH]What happen? (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
	return false;
}

bool check_clean_checkpoint_page(buf_page_t * bpage, bool is_single_page_flush){
	if(bpage->oldest_modification == 0 && bpage->buf_fix_count == 0 && buf_page_get_io_fix(bpage) == BUF_IO_NONE){
		if(bpage->is_iplized && !bpage->is_split_page){
			ipl_info * page_ipl_info = bpage->page_ipl_info;
			if(is_single_page_flush)	return true; // single_page flush인 경우
			if(page_ipl_info->dynamic_region_pointer != NULL){
				return true;
			}
		}
	}
	return false;
}
