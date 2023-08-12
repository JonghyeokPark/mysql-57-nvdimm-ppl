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
#include "buf0flu.h"
//싹다 클래스로 캡슐레이션을 해버려야 되나...
// 나중에 시간되면 해보기.

void alloc_static_ipl_to_bpage(buf_page_t * bpage){
	unsigned char * static_ipl_pointer = alloc_static_address_from_indirection_queue(buf_pool_get(bpage->id));
	unsigned char temp_buf[12] = {NULL, };
	if(static_ipl_pointer == NULL) return;
	ulint offset = 0;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.space());
	offset += 4;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.page_no());
	offset += 4;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, 0);
	offset += 4;

	//nvdimm에 작성
	memcpy(static_ipl_pointer, temp_buf, 12);
	flush_cache(static_ipl_pointer, 12);

	bpage->static_ipl_pointer = static_ipl_pointer;
	bpage->ipl_write_pointer = static_ipl_pointer + IPL_LOG_HEADER_SIZE;
	// fprintf(stderr, "Iplized,%f,%u,\n",(double)(time(NULL) - start),bpage->id.space());
	set_flag(&(bpage->flags), IPLIZED);
	// fprintf(stderr, "alloc static ipl (%u, %u) static_ipl: %p now_write_pointer: %p\n", bpage->id.space(), bpage->id.page_no(),bpage->static_ipl_pointer ,bpage->ipl_write_pointer);
}

void alloc_dynamic_ipl_region(buf_page_t * bpage){
	unsigned char * dynamic_address = alloc_dynamic_address_from_indirection_queue(buf_pool_get(bpage->id));
	if(dynamic_address == NULL)	return;
	unsigned char * pointer_to_store_dynamic_address = bpage->static_ipl_pointer + DYNAMIC_ADDRESS_OFFSET;
	mach_write_to_4(pointer_to_store_dynamic_address, get_ipl_index_from_addr(nvdimm_info->dynamic_start_pointer, dynamic_address, nvdimm_info->dynamic_ipl_per_page_size));
	flush_cache(pointer_to_store_dynamic_address, 4);
	// bpage->ipl_write_pointer = dynamic_address;
	// fprintf(stderr, "Dynamic,%f,%u,\n",(double)(time(NULL) - start),bpage->id.space());
	// fprintf(stderr, "Dynamic region allocated %p to (%u, %u) write_pointer: %p\n", dynamic_address, bpage->id.space(), bpage->id.page_no(), bpage->ipl_write_pointer);
}

void nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, ulint rest_log_len){
	// fprintf(stderr, "nvdimm_ipl_add(%u, %u) oldest_lsn: %lu flag: %d frmae: %p\n", bpage->id.space(), bpage->id.page_no(), bpage->oldest_modification,bpage->flags, ((buf_block_t *)bpage)->frame);
	unsigned char write_ipl_log_buffer [len] = {NULL, };

	ulint offset = 0;
	unsigned char store_type = type;
	unsigned short log_body_size = len - 3;

	mach_write_to_1(write_ipl_log_buffer + offset, store_type);
	offset += 1;
	mach_write_to_2(write_ipl_log_buffer + offset, log_body_size);
	offset += 2;
	//로그 하나 다 완성
	if(rest_log_len == 0){ // 다 들어갈 수 있는 경우
fit_size:
		memcpy(bpage->ipl_write_pointer, write_ipl_log_buffer, APPLY_LOG_HDR_SIZE);
		flush_cache(bpage->ipl_write_pointer, APPLY_LOG_HDR_SIZE);
		bpage->ipl_write_pointer += APPLY_LOG_HDR_SIZE;
		memcpy(bpage->ipl_write_pointer, log, log_body_size);
		flush_cache(bpage->ipl_write_pointer, log_body_size);
		bpage->ipl_write_pointer += log_body_size;
	}
	else{ // 다 들어갈 수 없는 경우
		ulint first_write_len = len - rest_log_len;
		if(first_write_len == 0){
			bpage->ipl_write_pointer = get_dynamic_ipl_pointer(bpage);
			goto fit_size;
		}
		memcpy(write_ipl_log_buffer + offset, log, log_body_size);

		memcpy(bpage->ipl_write_pointer, write_ipl_log_buffer, first_write_len);
		flush_cache(bpage->ipl_write_pointer, first_write_len);
		bpage->ipl_write_pointer = get_dynamic_ipl_pointer(bpage);
		memcpy(bpage->ipl_write_pointer, write_ipl_log_buffer + first_write_len, rest_log_len);
		flush_cache(bpage->ipl_write_pointer, rest_log_len);
		bpage->ipl_write_pointer += rest_log_len;
	}
	
}

bool can_write_in_ipl(buf_page_t * bpage, ulint log_len, ulint * rest_log_len){
	if(!get_flag(&(bpage->flags), IPLIZED)){
		alloc_static_ipl_to_bpage(bpage);
		return bpage->static_ipl_pointer != NULL;
	}
	unsigned char * dynamic_ipl_pointer = get_dynamic_ipl_pointer(bpage);
	ulint can_write_size;
	can_write_size = get_can_write_size_from_write_pointer(bpage);
	if(dynamic_ipl_pointer == NULL && log_len > can_write_size ){ // Static이 넘치는 경우
		alloc_dynamic_ipl_region(bpage);
		*rest_log_len = log_len - can_write_size;
		return get_dynamic_ipl_pointer(bpage) != NULL;
	}
	return log_len <= can_write_size;
}

void copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr){
	//NVDIMM 내 redo log를 메모리로 카피
	// fprintf(stderr, "apply_info! page_id:(%u, %u) static: %p dynamic: %p log_len: %zu\n", apply_info->space_id, apply_info->page_no, apply_info->static_start_pointer, apply_info->dynamic_start_pointer, apply_info->log_len);
	if(!(apply_info->dynamic_start_pointer == NULL)){
		//dynamic 영역이 존재하는 경우도 카피.
		ulint offset = 0;
		ulint apply_buffer_size = nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size;
		unsigned char temp_mtr_buffer [apply_buffer_size] = {NULL, };
		
		memcpy(temp_mtr_buffer + offset, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE ,nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		offset += nvdimm_info->static_ipl_per_page_size	- IPL_LOG_HEADER_SIZE;
		memcpy(temp_mtr_buffer + offset, apply_info->dynamic_start_pointer, nvdimm_info->dynamic_ipl_per_page_size);
		ipl_log_apply(temp_mtr_buffer, apply_info, temp_mtr);
		return;
	}
	else{
		ipl_log_apply(apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE, apply_info, temp_mtr);
	}
	
}

void ipl_log_apply(byte * apply_log_buffer, apply_log_info * apply_info, mtr_t * temp_mtr){
	byte * start_ptr = apply_log_buffer;
	byte * end_ptr = apply_info->dynamic_start_pointer == NULL ? apply_log_buffer + (nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE) : 
	apply_log_buffer + ((nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE) + nvdimm_info->dynamic_ipl_per_page_size);
	ulint now_len = 0;
	page_id_t page_id = apply_info->block->page.id;
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
		// fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu\n",page_id.space(), page_id.page_no(), log_type, body_len);

		//log apply 진행 후, recovery 시작 위치 이동.
		recv_parse_or_apply_log_rec_body(log_type, start_ptr, start_ptr + body_len, page_id.space(), page_id.page_no(), apply_info->block, temp_mtr);
		start_ptr += body_len;
	}
	now_len = start_ptr - apply_log_buffer;
apply_end:
	now_len += IPL_LOG_HEADER_SIZE;
	apply_info->block->page.ipl_write_pointer = apply_info->dynamic_start_pointer != NULL ? apply_info->dynamic_start_pointer + (now_len - nvdimm_info->static_ipl_per_page_size) : apply_info->static_start_pointer + now_len;
	temp_mtr->discard_modifications();
	mtr_commit(temp_mtr);
}

void set_apply_info_and_log_apply(buf_block_t* block) {
	buf_page_t * apply_page = (buf_page_t *)block;

	mtr_t temp_mtr;
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	page_id_t page_id = block->page.id;
	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying start!";

	apply_log_info apply_info;
	apply_info.static_start_pointer = apply_page->static_ipl_pointer;
	apply_info.dynamic_start_pointer = get_dynamic_ipl_pointer(apply_page);
	apply_info.block = block;

	// fprintf(stderr, "static pointer : %p, dynamic pointer : %p\n", apply_info.static_start_pointer, apply_info.dynamic_start_pointer);
	copy_log_to_mem_to_apply(&apply_info, &temp_mtr);
	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying finish!";
}


void insert_page_ipl_info_in_hash_table(buf_page_t * bpage){
	// fprintf(stderr, "insert lookup table(%u, %u) before oldest_lsn: %lu\n", bpage->id.space(), bpage->id.page_no(), bpage->oldest_modification);
	page_id_t page_id = bpage->id;
	std::pair <page_id_t, unsigned char *> insert_data = std::make_pair(bpage->id, bpage->static_ipl_pointer);
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	rw_lock_x_lock(&buf_pool->lookup_table_lock);
	buf_pool->ipl_look_up_table->insert(insert_data);
	rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	set_flag(&(bpage->flags), IN_LOOK_UP);
}

void nvdimm_ipl_add_split_merge_map(buf_page_t * bpage){
	if(!get_flag(&(bpage->flags), IN_LOOK_UP)){
		buf_pool_t * buf_pool = buf_pool_get(bpage->id);
		free_dynamic_address_to_indirection_queue(buf_pool, get_dynamic_ipl_pointer(bpage));
		free_static_address_to_indirection_queue(buf_pool, bpage->static_ipl_pointer);
		bpage->static_ipl_pointer = NULL;
		bpage->ipl_write_pointer = NULL;
		bpage->flags = 0;
	}
	set_flag(&(bpage->flags), NORMALIZE);
	// fprintf(stderr, "ipl_add Split page(%u, %u) before oldest_lsn: %lu flag: %d frmae: %p\n", bpage->id.space(), bpage->id.page_no(), bpage->oldest_modification, bpage->flags, ((buf_block_t *)bpage)->frame);
	// if(bpage->oldest_modification == 0){
	// 	buf_flush_note_modification((buf_block_t *)bpage, log_sys->lsn, log_sys->lsn, NULL);
	// }
	// fprintf(stderr, "ipl_add Split page(%u, %u) after oldest_lsn: %lu flag: %d frmae: %p \n", bpage->id.space(), bpage->id.page_no(), bpage->oldest_modification, bpage->flags, ((buf_block_t *)bpage)->frame);
	
}

void normalize_ipl_page(buf_page_t * bpage, page_id_t page_id){
	// fprintf(stderr, "ipl_remove page(%u, %u), static: %p, dynamic: %p\n", page_id.space(), page_id.page_no(), bpage->static_ipl_pointer, get_dynamic_ipl_pointer(bpage));
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		rw_lock_x_lock(&buf_pool->lookup_table_lock);
		buf_pool->ipl_look_up_table->erase(page_id);
		rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	}
	bpage->static_ipl_pointer = NULL;
	bpage->ipl_write_pointer = NULL;
	bpage->flags = 0;
}



void set_for_ipl_page(buf_page_t* bpage){
	bpage->static_ipl_pointer = NULL;
	// fprintf(stderr, "Read page! page_id(%u, %u)\n", bpage->id.space(), bpage->id.page_no());
	bpage->flags = 0;
	bpage->ipl_write_pointer = NULL;
	page_id_t page_id = bpage->id;
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	rw_lock_s_lock(&buf_pool->lookup_table_lock);
	std::tr1::unordered_map<page_id_t, unsigned char * >::iterator it = buf_pool->ipl_look_up_table->find(page_id);
	rw_lock_s_unlock(&buf_pool->lookup_table_lock);
	if(it != buf_pool->ipl_look_up_table->end()){
		// fprintf(stderr, "Read ipl page! page_id(%u, %u)\n", bpage->id.space(), bpage->id.page_no());
		set_flag(&(bpage->flags), IPLIZED);
		set_flag(&(bpage->flags), IN_LOOK_UP);
		bpage->static_ipl_pointer = it->second;
	}
	
}


//Dynamic 영역을 가지고 있는 checkpoint page인지 확인하기.
bool check_not_flush_page(buf_page_t * bpage, buf_flush_t flush_type){
	// fprintf(stderr, "Flush(%u, %u), old_lsn: %zu, buf_fix_count: %u, io_fix: %u, frame: %p\n", bpage->id.space(), bpage->id.page_no(), bpage->oldest_modification, bpage->buf_fix_count, buf_page_get_io_fix(bpage), ((buf_block_t *)bpage)->frame);
	if(get_flag(&(bpage->flags), IPLIZED) == false){
		// fprintf(stderr, "[FLUSH] Normal page: (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
		return false;
	}
	else{
		if(get_flag(&(bpage->flags), NORMALIZE)){
			// fprintf(stderr, "[FLUSH]split ipl page: (%u, %u), flush_type: %d\n", bpage->id.space(), bpage->id.page_no(), flush_type);
			return false;
		}
		if(get_dynamic_ipl_pointer(bpage)== NULL){
			fprintf(stderr, "[Not Flush]Only Static ipl page: (%u, %u) flush_type %u, old_lsn: %zu, buf_fix_count: %u, io_fix: %u, frmae: %p\n", bpage->id.space(), bpage->id.page_no(), flush_type, bpage->oldest_modification, bpage->buf_fix_count, buf_page_get_io_fix(bpage), ((buf_block_t *)bpage)->frame);
			return true;
		}
		else{
			if(flush_type == BUF_FLUSH_LIST){
				fprintf(stderr, "[Not Flush]Checkpoint ipl page: (%u, %u) flush_type %u\n", bpage->id.space(), bpage->id.page_no(), flush_type);
				return true;
			}
			else{
				return false;
			}

		}

	}
	return false;
}
static inline void print_flush_type(buf_flush_t flush_type, bool iplized){
	if(iplized){
		switch(flush_type){
			case BUF_FLUSH_LIST:
				fprintf(stderr, "%f,ipl_checkpoint_flush\n",(double)(time(NULL) - start));
				break;
			case BUF_FLUSH_SINGLE_PAGE:
				fprintf(stderr, "%f,ipl_single_page_flush\n",(double)(time(NULL) - start));
				break;
			case BUF_FLUSH_LRU:
				fprintf(stderr, "%f,ipl_lru_flush\n",(double)(time(NULL) - start));
				break;
		}
	}
	else{
		switch(flush_type){
			case BUF_FLUSH_LIST:
				fprintf(stderr, "%f,normal_checkpoint_flush\n",(double)(time(NULL) - start));
				break;
			case BUF_FLUSH_SINGLE_PAGE:
				fprintf(stderr, "%f,normal_single_page_flush\n",(double)(time(NULL) - start));
				break;
			case BUF_FLUSH_LRU:
				fprintf(stderr, "%f,normal_lru_flush\n",(double)(time(NULL) - start));
				break;
		}
	}
}

bool check_have_to_normalize_page_and_normalize(buf_page_t * bpage, buf_flush_t flush_type){
	if(get_flag(&(bpage->flags), IPLIZED) == false){
		// print_flush_type(flush_type, false);
		return false;
	}
	else{
		if(get_flag(&(bpage->flags), NORMALIZE)){
			// print_flush_type(flush_type, false);
			normalize_ipl_page(bpage, bpage->id);
			return true;
		}
		if(get_dynamic_ipl_pointer(bpage) == NULL){
			// print_flush_type(flush_type, true);
			return false;
		}
		else{
			if(flush_type == BUF_FLUSH_LIST){
				// print_flush_type(flush_type, true);
				return false;
			}
			else{
				// print_flush_type(flush_type, false);
				normalize_ipl_page(bpage, bpage->id);
				return true;
				
			}

		}

	}
}

bool check_clean_checkpoint_page(buf_page_t * bpage, bool is_single_page_flush){
	if(bpage->oldest_modification == 0 && bpage->buf_fix_count == 0 && buf_page_get_io_fix(bpage) == BUF_IO_NONE){
		if(get_flag(&(bpage->flags), IPLIZED) && !get_flag(&(bpage->flags), NORMALIZE)){
			if(get_dynamic_ipl_pointer(bpage) != NULL){
				return true;
			}
		}
	}
	return false;
}

ulint get_can_write_size_from_write_pointer(buf_page_t * bpage){
	unsigned char * static_ipl_pointer = bpage->static_ipl_pointer;
	unsigned char * dynamic_ipl_pointer = get_dynamic_ipl_pointer(bpage);
	unsigned char * write_pointer = bpage->ipl_write_pointer;
	return dynamic_ipl_pointer != NULL ? (nvdimm_info->dynamic_ipl_per_page_size - (write_pointer - dynamic_ipl_pointer)) : (nvdimm_info->static_ipl_per_page_size - (write_pointer - static_ipl_pointer));
}

unsigned char * get_dynamic_ipl_pointer(buf_page_t * bpage){
	// fprintf(stderr, "get_dynamic_ipl_pointer (%u, %u) static_ipl: %p now_write_pointer: %p\n", bpage->id.space(), bpage->id.page_no(),bpage->static_ipl_pointer ,bpage->ipl_write_pointer);
	if(bpage->static_ipl_pointer == NULL) return NULL;
	uint ipl_index = mach_read_from_4(bpage->static_ipl_pointer + DYNAMIC_ADDRESS_OFFSET);
	return get_addr_from_ipl_index(nvdimm_info->dynamic_start_pointer, ipl_index, nvdimm_info->dynamic_ipl_per_page_size);
}

void set_flag(unsigned char * flags, ipl_flag flag_type){
	(*flags) |= flag_type;
}
void unset_flag(unsigned char * flags, ipl_flag flag_type){
	(*flags) &= ~flag_type;
}
bool get_flag(unsigned char * flags, ipl_flag flag_type){
	return (*flags) & flag_type;
}