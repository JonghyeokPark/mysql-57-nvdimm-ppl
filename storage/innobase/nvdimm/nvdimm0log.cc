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

bool alloc_static_ipl_to_bpage(buf_page_t * bpage){
	unsigned char * static_ipl_pointer = alloc_static_address_from_indirection_queue(buf_pool_get(bpage->id));
	unsigned char temp_buf[8] = {NULL, };
	if(static_ipl_pointer == NULL) return false;
	ulint offset = 0;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.space()); // Store Space id
	offset += 4;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.page_no());// Store Page_no
	offset += 4;

	//nvdimm에 작성
	memcpy(static_ipl_pointer, temp_buf, 8);
	flush_cache(static_ipl_pointer, 8);

	//IPL Pointer 설정
	bpage->static_ipl_pointer = static_ipl_pointer;
	bpage->ipl_write_pointer = static_ipl_pointer + IPL_LOG_HEADER_SIZE;

	set_flag(&(bpage->flags), IPLIZED);
	// fprintf(stderr, "alloc static ipl (%u, %u) static_ipl: %p now_write_pointer: %p\n", bpage->id.space(), bpage->id.page_no(),bpage->static_ipl_pointer ,bpage->ipl_write_pointer);
	return true;
}

bool alloc_dynamic_ipl_to_bpage(buf_page_t * bpage){
	//First DIPL 할당 시도
	if(get_dynamic_ipl_pointer(bpage) != NULL)	return true;
	unsigned char * dynamic_address = alloc_dynamic_address_from_indirection_queue(buf_pool_get(bpage->id));
	if(dynamic_address == NULL)	return false;

	//Static 영역에 First DIPL index 저장
	unsigned char * pointer_to_store_dynamic_address = bpage->static_ipl_pointer + DYNAMIC_ADDRESS_OFFSET;
	mach_write_to_4(pointer_to_store_dynamic_address, get_ipl_index_from_addr(nvdimm_info->dynamic_start_pointer, dynamic_address, nvdimm_info->dynamic_ipl_per_page_size));
	flush_cache(pointer_to_store_dynamic_address, 4);

	// fprintf(stderr, "Dynamic region allocated %p to (%u, %u) now_write_pointer: %p\n", dynamic_address, bpage->id.space(), bpage->id.page_no(), bpage->ipl_write_pointer);
	return true;
}

bool alloc_second_dynamic_ipl_to_bpage(buf_page_t * bpage){
	//Second DIPL 할당 시도
	if(get_second_dynamic_ipl_pointer(bpage) != NULL){
		set_flag(&(bpage->flags), SECOND_DIPL);
		return true;
	}
	unsigned char * second_dynamic_address = alloc_second_dynamic_address_from_indirection_queue(buf_pool_get(bpage->id));
	if(second_dynamic_address == NULL)	return false;
	
	//First DIPL 영역에 Second DIPL Index 저장
	unsigned char * pointer_to_store_dynamic_address = get_dynamic_ipl_pointer(bpage);
	mach_write_to_4(pointer_to_store_dynamic_address, get_ipl_index_from_addr(nvdimm_info->second_dynamic_start_pointer, second_dynamic_address, nvdimm_info->second_dynamic_ipl_per_page_size));
	flush_cache(pointer_to_store_dynamic_address, 4);

	set_flag(&(bpage->flags), SECOND_DIPL);
	// fprintf(stderr, "Second Dynamic region allocated %p to (%u, %u) write_pointer: %p\n", second_dynamic_address, bpage->id.space(), bpage->id.page_no(), bpage->ipl_write_pointer);
	return true;
}

/* Sjmun IPL Log를 쓸때의 핵심*/
/* SIPL 영역 공간이 부족한 경우는 2개로 나누어서 기존 IPL 영역, 새로운 IPL 영역에 작성하게 됨*/
/* Log write atomicity를 보장하기 위해서는 두번째 파트를 다 작성하고 Length를 Flush cache*/

void nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, ulint rest_log_len, trx_id_t trx_id){
	unsigned char write_ipl_log_buffer [len] = {NULL, };
	uint test;
	ulint offset = 0;
	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;
	// fprintf(stderr, "nvdimm_ipl_add(%u, %u) Log type: %d, log_len: %lu, trx_id: %zu now_write_pointer: %p can_write_size: %lu\n", bpage->id.space(), bpage->id.page_no(), type, log_body_size, trx_id, bpage->ipl_write_pointer, get_can_write_size_from_write_pointer(bpage, &test));

	//Step1. Apply log header 작성
	mach_write_to_1(write_ipl_log_buffer + offset, store_type); // mtr_log type
	offset += 1;
	mach_write_to_2(write_ipl_log_buffer + offset, log_body_size); //mtr_log body
	offset += 2;
	mach_write_to_8(write_ipl_log_buffer + offset, trx_id); // mtr_log trx_id
	offset += 8;

	//Step2. Header + Body가 한 영역에 다 들어갈 수 있는지 확인
	if(rest_log_len == 0){ 	//Header + Body가 한 영역에 다 들어갈 수 있는 경우
fit_size:
		//Step3. Header 내용 IPL에 작성
		memcpy(bpage->ipl_write_pointer, write_ipl_log_buffer, APPLY_LOG_HDR_SIZE);
		flush_cache(bpage->ipl_write_pointer, APPLY_LOG_HDR_SIZE);
		bpage->ipl_write_pointer += APPLY_LOG_HDR_SIZE;
		//Setp4. Body 내용 IPL에 작성
		memcpy(bpage->ipl_write_pointer, log, log_body_size);
		flush_cache(bpage->ipl_write_pointer, log_body_size);
		bpage->ipl_write_pointer += log_body_size;
		set_ipl_length_in_ipl_header(bpage);
		
		return;
	}
	else{ //Header + Body가 한 영역에 다 들어갈 수 없는 경우
		ulint first_write_len = len - rest_log_len;
		if(first_write_len == 0){ // 현재 IPL에 쓸 수 있는 공간이 없는 경우, e.g. Static 256B를 다 쓴 경우
			if(!get_flag(&(bpage->flags), SECOND_DIPL)){
				bpage->ipl_write_pointer = get_dynamic_ipl_pointer(bpage);
				bpage->ipl_write_pointer += DIPL_HEADER_SIZE;
			}
			else{ 
				bpage->ipl_write_pointer = get_second_dynamic_ipl_pointer(bpage);
			}
			goto fit_size;
		}
		//Step 3. Header + Body 붙이기
		memcpy(write_ipl_log_buffer + offset, log, log_body_size);

		//Step 4. 남은 IPL 공간에 1차로 작성
		memcpy(bpage->ipl_write_pointer, write_ipl_log_buffer, first_write_len);
		flush_cache(bpage->ipl_write_pointer, first_write_len);
		bpage->ipl_write_pointer += first_write_len;
		
		//Setp 5. 할당받은 IPL 공간으로 Write pointer 재설정
		if(!get_flag(&(bpage->flags), SECOND_DIPL)){
			bpage->ipl_write_pointer = get_dynamic_ipl_pointer(bpage);
			bpage->ipl_write_pointer += DIPL_HEADER_SIZE;
		}
		else{ 
			bpage->ipl_write_pointer = get_second_dynamic_ipl_pointer(bpage);
		}
		
		// fprintf(stderr, "Second Write(%u, %u) Log type: %d, rest_log_len: %lu now_write_pointer: %p can_write_size: %lu\n", bpage->id.space(), bpage->id.page_no(), type, rest_log_len, bpage->ipl_write_pointer, get_can_write_size_from_write_pointer(bpage, &test));

		//Step 6. 할당받은 IPL 공간에 2차로 작성
		memcpy(bpage->ipl_write_pointer, write_ipl_log_buffer + first_write_len, rest_log_len);
		flush_cache(bpage->ipl_write_pointer, rest_log_len);
		bpage->ipl_write_pointer += rest_log_len;
		set_ipl_length_in_ipl_header(bpage);
		
	}
	
}

bool can_write_in_ipl(buf_page_t * bpage, ulint log_len, ulint * rest_log_len){
	//IPL 할당받지 못한 페이지는 IPL 할당 시도
	if(!get_flag(&(bpage->flags), IPLIZED)){
		return alloc_static_ipl_to_bpage(bpage);
	}
	unsigned char * dynamic_ipl_pointer = get_dynamic_ipl_pointer(bpage);
	ulint can_write_size;
	uint type; 
	can_write_size = get_can_write_size_from_write_pointer(bpage, &type);
	switch (type) // 쓰고있는 영역 0: Static 영역, 1: First DIPL 영역, 2: Second DIPL 영역
	{
	case 0:
		if(log_len > can_write_size){
			*rest_log_len = log_len - can_write_size;
			return alloc_dynamic_ipl_to_bpage(bpage);
		}
		break;
	case 1:
		if(log_len > can_write_size){
			*rest_log_len = log_len - can_write_size;
			return alloc_second_dynamic_ipl_to_bpage(bpage);
		}
		break;
	default:
		break;
	}
	return log_len <= can_write_size;
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
	apply_info.second_dynamic_start_pointer = get_second_dynamic_ipl_pointer(apply_page);
	apply_info.ipl_log_length = get_ipl_length_from_ipl_header(apply_page);
	apply_info.block = block;

	// fprintf(stderr, "static pointer : %p, dynamic pointer : %p\n", apply_info.static_start_pointer, apply_info.dynamic_start_pointer);
	copy_log_to_mem_to_apply(&apply_info, &temp_mtr);
	// ib::info() << "(" << page_id.space() << ", " << page_id.page_no()  << ")" <<  " IPL applying finish!";
}

/* Header에 있는 valid한 ipl_log_length를 기준으로 apply 시도 */
/* 만약, IPL Log를 작성하다 Failure 발생시, Log length는 업데이트 되기 전이기에 Atomicity 보장 */
void copy_log_to_mem_to_apply(apply_log_info * apply_info, mtr_t * temp_mtr){
	ulint offset, sipl_size, dipl_size, sdipl_size, apply_buffer_size;
	offset = 0;
	apply_buffer_size = 0;
	sipl_size = nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE;
	dipl_size = nvdimm_info->dynamic_ipl_per_page_size - DIPL_HEADER_SIZE;

	//Step 1. Second DIPL, First DIPL, SIPL apply인지 판별
	if(apply_info->ipl_log_length > nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size){
		// fprintf(stderr, "SDIPL apply! page_id:(%u, %u) static: %p dynamic: %p second_dynamic:%p\n", apply_info->block->page.id.space(), apply_info->block->page.id.page_no(), apply_info->static_start_pointer, apply_info->dynamic_start_pointer, apply_info->second_dynamic_start_pointer);

		//Step 2. SIPL, First DIPL, Second DIPL 순서로 copy해서 연결
		sdipl_size = apply_info->ipl_log_length - nvdimm_info->static_ipl_per_page_size - nvdimm_info->dynamic_ipl_per_page_size;
		apply_buffer_size = sipl_size + dipl_size + sdipl_size;

		unsigned char temp_mtr_buffer [apply_buffer_size] = {NULL, };
		memcpy(temp_mtr_buffer, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE ,sipl_size);
		offset += sipl_size;
		memcpy(temp_mtr_buffer + offset, apply_info->dynamic_start_pointer + DIPL_HEADER_SIZE, dipl_size);
		offset += dipl_size;
		memcpy(temp_mtr_buffer + offset, apply_info->second_dynamic_start_pointer, sdipl_size);

		//Step 4. IPL log apply
		ipl_log_apply(temp_mtr_buffer, temp_mtr_buffer + apply_buffer_size, apply_info, temp_mtr);
		return;
	}
	else if(apply_info->ipl_log_length > nvdimm_info->static_ipl_per_page_size){
		// fprintf(stderr, "DIPL apply! page_id:(%u, %u) static: %p dynamic: %p second_dynamic:%p\n", apply_info->block->page.id.space(), apply_info->block->page.id.page_no(), apply_info->static_start_pointer, apply_info->dynamic_start_pointer, apply_info->second_dynamic_start_pointer);
		//Step 2. SIPL, First DIPL 순서로 copy해서 연결
		dipl_size = apply_info->ipl_log_length - nvdimm_info->static_ipl_per_page_size;
		apply_buffer_size = sipl_size + dipl_size;
		
		unsigned char temp_mtr_buffer [apply_buffer_size] = {NULL, };
		memcpy(temp_mtr_buffer, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE ,sipl_size);
		offset += sipl_size;
		memcpy(temp_mtr_buffer + offset, apply_info->dynamic_start_pointer + DIPL_HEADER_SIZE, dipl_size);

		//Step 3. IPL log apply
		ipl_log_apply(temp_mtr_buffer,temp_mtr_buffer + apply_buffer_size, apply_info, temp_mtr);
		return;
	}
	else{
		//Step 3. IPL log apply
		sipl_size = apply_info->ipl_log_length - IPL_LOG_HEADER_SIZE;
		//Setp 3. 만약, SIPL 영역을 할당만받고 헤더만 작성한 경우, SIPL apply로 전환
		if(dipl_size > nvdimm_info->dynamic_ipl_per_page_size){
			return;
		}
		ipl_log_apply(apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE, apply_info->static_start_pointer + IPL_LOG_HEADER_SIZE + sipl_size, apply_info, temp_mtr);
	}
	
}

void ipl_log_apply(byte * start_ptr, byte * end_ptr, apply_log_info * apply_info, mtr_t * temp_mtr){
	// fprintf(stderr,"Apply! (%u, %u) start_ptr: %p, end_ptr: %p\n", apply_info->block->page.id.space(), apply_info->block->page.id.page_no(), start_ptr, end_ptr);
	byte * apply_ptr = start_ptr;
	page_id_t page_id = apply_info->block->page.id;
	while (apply_ptr < end_ptr) {
		// Step 1. Log type 읽어오기
		mlog_id_t log_type = mlog_id_t(mach_read_from_1(apply_ptr));
		apply_ptr += 1;
		//Setp 2. log body length를 읽기
		ulint body_len = mach_read_from_2(apply_ptr);
		apply_ptr += 2;
		//Setp 3. Trx id 읽어오기
		trx_id_t trx_id = mach_read_from_8(apply_ptr);
		apply_ptr += 8;
		if(log_type == MLOG_COMP_PAGE_CREATE){
			apply_ptr += body_len;
			continue;
		}

		recv_parse_or_apply_log_rec_body(log_type, apply_ptr, apply_ptr + body_len, page_id.space(), page_id.page_no(), apply_info->block, temp_mtr);
		apply_ptr += body_len;
		// fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu, apply_ptr: %p, start_ptr: %p\n",page_id.space(), page_id.page_no(), log_type, body_len, apply_ptr, start_ptr);
	}
	//Step 4. IPL apply가 끝난 후 apply_ptr을 기준으로 write_pointer 재설정
	if(apply_info->ipl_log_length > nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size){
		apply_info->block->page.ipl_write_pointer = apply_info->second_dynamic_start_pointer + (apply_info->ipl_log_length - nvdimm_info->static_ipl_per_page_size - nvdimm_info->dynamic_ipl_per_page_size);
	}
	else if(apply_info->ipl_log_length > nvdimm_info->static_ipl_per_page_size){
		apply_info->block->page.ipl_write_pointer = apply_info->dynamic_start_pointer + (apply_info->ipl_log_length - nvdimm_info->static_ipl_per_page_size);
	}
	else{
		apply_info->block->page.ipl_write_pointer = apply_info->static_start_pointer + apply_info->ipl_log_length;
	}
	temp_mtr->discard_modifications();
	mtr_commit(temp_mtr);
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

/* TODO Sjmun : 한 번도 Discard되지 않은 페이지들은 사실 IPL을 사용할 필요 없이 Global redo로그로만 복구가능한데..  */
void nvdimm_ipl_add_split_merge_map(buf_page_t * bpage){
	// /*Step 1. Page가 IPL화 되고 한 번도 Discard도 되진 않은 경우는 바로 Normalize 시도*/
	// if(!get_flag(&(bpage->flags), NORMALIZE) && get_flag(&(bpage->flags), IPLIZED)){
		// fprintf(stderr, "flag: %d\n",bpage->flags);
		// set_normalize_flag_in_ipl_header(bpage->static_ipl_pointer);
		// if(!get_flag(&(bpage->flags), IN_LOOK_UP)){
		// 	buf_pool_t * buf_pool = buf_pool_get(bpage->id);
		// 	free_second_dynamic_address_to_indirection_queue(buf_pool, get_second_dynamic_ipl_pointer(bpage));
		// 	free_dynamic_address_to_indirection_queue(buf_pool, get_dynamic_ipl_pointer(bpage));
		// 	free_static_address_to_indirection_queue(buf_pool, bpage->static_ipl_pointer);
		// 	bpage->static_ipl_pointer = NULL;
		// 	bpage->ipl_write_pointer = NULL;
		// 	bpage->flags = 0; // flag를 전부 초기화 하느냐, normalize flag만 세우냐 그 차이
		// }
	// }
	set_flag(&(bpage->flags), NORMALIZE);
}

/* Unset_flag를 해주지 않아도 Static_ipl이 free 되면 초기화 됨*/
void normalize_ipl_page(buf_page_t * bpage, page_id_t page_id){ 
	// fprintf(stderr, "Normalize (%u, %u), static: %p, dynamic: %p\n", page_id.space(), page_id.page_no(), bpage->static_ipl_pointer, get_dynamic_ipl_pointer(bpage));
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		rw_lock_x_lock(&buf_pool->lookup_table_lock);
		buf_pool->ipl_look_up_table->erase(page_id);
		rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	}
	bpage->static_ipl_pointer = NULL;
	bpage->ipl_write_pointer = NULL;
	bpage->flags = 0;
	//nvdimm add_trx_id
	bpage->trx_id = 0;
	//nvdimm add_trx_id
}



void set_for_ipl_page(buf_page_t* bpage){
	//nvdimm add_trx_id
	bpage->trx_id = 0;
	//nvdimm add_trx_id
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
		// fprintf(stderr, "Read ipl page! page_id(%u, %u) lsn: %lu\n", bpage->id.space(), bpage->id.page_no(), get_page_lsn_from_ipl_header(bpage->static_ipl_pointer));
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
				// fprintf(stderr, "[Not Flush]LRU ipl page: (%u, %u) flush_type %u\n", bpage->id.space(), bpage->id.page_no(), flush_type);
				return false;
			}

		}

	}
	return false;
}
// static inline void print_flush_type(buf_flush_t flush_type, bool iplized){
// 	if(iplized){
// 		switch(flush_type){
// 			case BUF_FLUSH_LIST:
// 				fprintf(stderr, "%f,ipl_checkpoint_flush\n",(double)(time(NULL) - start));
// 				break;
// 			case BUF_FLUSH_SINGLE_PAGE:
// 				fprintf(stderr, "%f,ipl_single_page_flush\n",(double)(time(NULL) - start));
// 				break;
// 			case BUF_FLUSH_LRU:
// 				fprintf(stderr, "%f,ipl_lru_flush\n",(double)(time(NULL) - start));
// 				break;
// 		}
// 	}
// 	else{
// 		switch(flush_type){
// 			case BUF_FLUSH_LIST:
// 				fprintf(stderr, "%f,normal_checkpoint_flush\n",(double)(time(NULL) - start));
// 				break;
// 			case BUF_FLUSH_SINGLE_PAGE:
// 				fprintf(stderr, "%f,normal_single_page_flush\n",(double)(time(NULL) - start));
// 				break;
// 			case BUF_FLUSH_LRU:
// 				fprintf(stderr, "%f,normal_lru_flush\n",(double)(time(NULL) - start));
// 				break;
// 		}
// 	}
// }

bool check_have_to_normalize_page_and_normalize(buf_page_t * bpage, buf_flush_t flush_type){
	if(get_flag(&(bpage->flags), IPLIZED) == false){
		// print_flush_type(flush_type, false);
		//nvdimm add_trx_id
		bpage->trx_id = 0;
		bpage->static_ipl_pointer = NULL;
		bpage->flags = 0;
		bpage->ipl_write_pointer = NULL;
		//nvdimm add_trx_id
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

ulint get_can_write_size_from_write_pointer(buf_page_t * bpage, uint * type){
	unsigned char * static_ipl_pointer = bpage->static_ipl_pointer;
	unsigned char * dynamic_ipl_pointer = get_dynamic_ipl_pointer(bpage);
	unsigned char * second_dynamic_ipl_pointer = get_second_dynamic_ipl_pointer(bpage);
	unsigned char * write_pointer = bpage->ipl_write_pointer;
	ulint return_value = 0;
	if(dynamic_ipl_pointer == NULL || (dynamic_ipl_pointer != NULL && write_pointer < dynamic_ipl_pointer)){ 
		return_value = nvdimm_info->static_ipl_per_page_size - (write_pointer - static_ipl_pointer);
		*type = 0;
	}
	else if(second_dynamic_ipl_pointer == NULL || (second_dynamic_ipl_pointer != NULL && write_pointer < second_dynamic_ipl_pointer)){
		return_value = nvdimm_info->dynamic_ipl_per_page_size - (write_pointer - dynamic_ipl_pointer);
		*type = 1;
	}
	else{
		return_value = nvdimm_info->second_dynamic_ipl_per_page_size - (write_pointer - second_dynamic_ipl_pointer);
		*type = 2;
	}
	return return_value;
}

ulint get_ipl_length_from_write_pointer(buf_page_t * bpage){
	ulint return_value;
	unsigned char * static_ipl_pointer = bpage->static_ipl_pointer;
	unsigned char * dynamic_ipl_pointer = get_dynamic_ipl_pointer(bpage);
	unsigned char * second_dynamic_ipl_pointer = get_second_dynamic_ipl_pointer(bpage);
	unsigned char * write_pointer = bpage->ipl_write_pointer;
	if(dynamic_ipl_pointer == NULL || (dynamic_ipl_pointer != NULL && write_pointer < dynamic_ipl_pointer)){ 
		return_value = (write_pointer - static_ipl_pointer);
	}
	else if(second_dynamic_ipl_pointer == NULL || (second_dynamic_ipl_pointer != NULL && write_pointer < second_dynamic_ipl_pointer)){
		return_value = (write_pointer - dynamic_ipl_pointer) + nvdimm_info->static_ipl_per_page_size;
	}
	else{
		return_value = (write_pointer - second_dynamic_ipl_pointer) + nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size;;
	}
	return return_value;
}

unsigned char * get_dynamic_ipl_pointer(buf_page_t * bpage){
	// fprintf(stderr, "get_dynamic_ipl_pointer (%u, %u) static_ipl: %p now_write_pointer: %p\n", bpage->id.space(), bpage->id.page_no(),bpage->static_ipl_pointer ,bpage->ipl_write_pointer);
	if(bpage->static_ipl_pointer == NULL) return NULL;
	uint ipl_index = mach_read_from_4(bpage->static_ipl_pointer + DYNAMIC_ADDRESS_OFFSET);
	return get_addr_from_ipl_index(nvdimm_info->dynamic_start_pointer, ipl_index, nvdimm_info->dynamic_ipl_per_page_size);
}

unsigned char * get_second_dynamic_ipl_pointer(buf_page_t * bpage){
	// fprintf(stderr, "get_second_dynamic_ipl_pointer (%u, %u) static_ipl: %p now_write_pointer: %p\n", bpage->id.space(), bpage->id.page_no(),bpage->static_ipl_pointer ,bpage->ipl_write_pointer);
	unsigned char * dynamic_ipl_pointer = get_dynamic_ipl_pointer(bpage);
	if(dynamic_ipl_pointer == NULL) return NULL;
	uint ipl_index = mach_read_from_4(dynamic_ipl_pointer);
	return get_addr_from_ipl_index(nvdimm_info->second_dynamic_start_pointer, ipl_index, nvdimm_info->second_dynamic_ipl_per_page_size);
}

void set_ipl_length_in_ipl_header(buf_page_t * bpage){
	unsigned char * static_ipl_pointer = bpage->static_ipl_pointer;
	mach_write_to_4(static_ipl_pointer + IPL_LENGTH_OFFSET, get_ipl_length_from_write_pointer(bpage));
	flush_cache(static_ipl_pointer + IPL_LENGTH_OFFSET, 4);
}

uint get_ipl_length_from_ipl_header(buf_page_t * bpage){
	return mach_read_from_4(bpage->static_ipl_pointer + IPL_LENGTH_OFFSET);
}

void set_page_lsn_in_ipl_header(unsigned char* static_ipl_pointer, lsn_t lsn){
  // (jhpark): recovery
  if (nvdimm_recv_running) return;
	mach_write_to_8(static_ipl_pointer + IPL_PAGE_LSN_OFFSET, lsn);
	flush_cache(static_ipl_pointer + IPL_PAGE_LSN_OFFSET, 8);
}

lsn_t get_page_lsn_from_ipl_header(unsigned char* static_ipl_pointer){
	return mach_read_from_8(static_ipl_pointer + IPL_PAGE_LSN_OFFSET);
}

void set_normalize_flag_in_ipl_header(unsigned char * static_ipl_pointer){
	set_flag(static_ipl_pointer + IPL_FLAG_OFFSET, NORMALIZE);
	flush_cache(static_ipl_pointer + IPL_FLAG_OFFSET, 1);
}

unsigned char * get_flag_in_ipl_header(unsigned char * static_ipl_pointer){
	return static_ipl_pointer + IPL_FLAG_OFFSET;
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

/* lbh */
dberr_t
nvdimm_build_prev_vers_with_redo(
	const rec_t*	rec,		/*!< in: record in a clustered index */
	mtr_t*		mtr,
	dict_index_t*	clust_index,	/*!< in: clustered index */
	ulint**		offsets,	/*!< in/out: offsets returned by
					rec_get_offsets(rec, clust_index) */
	ReadView*	read_view,	/*!< in: read view */
	mem_heap_t**	offset_heap,	/*!< in/out: memory heap from which
					the offsets are allocated */
	mem_heap_t*	in_heap,/*!< in: memory heap from which the memory for
				*old_vers is allocated; memory for possible
				intermediate versions is allocated and freed
				locally within the function */
	rec_t**		old_vers,	/*!< out: old version, or NULL if the
					record does not exist in the view:
					i.e., it was freshly inserted
					afterwards */
	const dtuple_t**vrow,		/*!< out: dtuple to hold old virtual
					column data */
	buf_page_t* bpage ){

	page_id_t page_id = bpage->id; // cur bpage 
	buf_block_t* block = buf_page_get_block(bpage); // cur block
	
	
	//fprintf(stderr, "Start version build with IPL space_id: %d page_no: %lu bpage: %lu\n", bpage->id.space(), bpage->id.page_no(), bpage);
	
	/* 0. Get the IPL info of the page that transaction is about to read */
	
	apply_log_info apply_info;
	apply_info.static_start_pointer = bpage->static_ipl_pointer;
	apply_info.dynamic_start_pointer = get_dynamic_ipl_pointer(bpage);
	apply_info.block = block;

	/* 1. Read the old data page from the disk for page-level version build */

	page_t* page = block->frame;
	UNIV_MEM_ASSERT_RW(page, UNIV_PAGE_SIZE);
	//buf_pool_t*	buf_pool	= buf_pool_from_bpage(&block->page);
	ulint id = dict_index_get_space(clust_index);
	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(id, &found));

	byte*	buf2;
	byte*	temp_page;
	dberr_t		err = DB_SUCCESS;
	//buf_block_t* old_block;
	bool resuse_prev_built_page = false;
	trx_id_t	old_page_max_trx_id;
	trx_id_t	cur_page_max_trx_id;
	trx_id_t temp_trx_id;
	mtr_t temp_mtr;
	byte* start_ptr;
	byte* end_ptr;
	ulint cur_len = 0;
	int IPL_apply_cnt = 0;
	byte* temp_old_page_ptr = nvdimm_info->old_page;
	byte* buf1;
	byte* old_page;
	ulint final_ipl_log;
	ulint ipl_log_offset;
	rec_t* final_ipl_rec = NULL;
	ulint* final_ipl_rec_offset;
	rec_t* ipl_log_to_rec;
	mem_heap_t* ipl_rec_heap = mem_heap_create(256);


	ulint			clust_pos = 0;
	ulint			clust_len;
	const dict_col_t*	col;
	const byte*	clust_field;
	ulint		clust_offsets_[REC_OFFS_NORMAL_SIZE];
	ulint*		clust_offs	= clust_offsets_;
	mem_heap_t*	heap		= NULL;
	const byte* rec1_b_ptr;
	const byte* rec2_b_ptr;
	bool ret = false;
	int apply_rec_cnt = 0;

	

	buf2 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	temp_page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));


	if(nvdimm_info->old_page==NULL){
		nvdimm_info->old_page = (byte *)calloc(16384, sizeof(char));
		buf_frame_copy(nvdimm_info->old_page, page);
	}

	if(page_get_page_no(nvdimm_info->old_page)==page_get_page_no(page) && temp_old_page_ptr!=NULL){
		resuse_prev_built_page = true;
		goto get_rec_offset;
	}

	/* 1. Copy IPL region to memory */

	byte * apply_log_buffer;
	
	if(apply_info.dynamic_start_pointer == NULL){
		apply_log_buffer = (byte *)calloc(nvdimm_info->static_ipl_per_page_size, sizeof(char));
		memcpy(apply_log_buffer, apply_info.static_start_pointer + IPL_LOG_HEADER_SIZE, nvdimm_info->static_ipl_per_page_size);
		flush_cache(apply_log_buffer, nvdimm_info->static_ipl_per_page_size);
		//fprintf(stderr, "static_region_size: %u\n", nvdimm_info->static_ipl_per_page_size);

	}else{ //Copy dynamic region
		ulint offset = 0;
		ulint apply_buffer_size = nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size;
		apply_log_buffer = (byte *)calloc(apply_buffer_size, sizeof(char));

		memcpy(apply_log_buffer + offset, apply_info.static_start_pointer + IPL_LOG_HEADER_SIZE ,nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		flush_cache(apply_log_buffer + offset, nvdimm_info->static_ipl_per_page_size - IPL_LOG_HEADER_SIZE);
		offset += nvdimm_info->static_ipl_per_page_size	- IPL_LOG_HEADER_SIZE;
		//fprintf(stderr, "dynamic_region_size: %u\n", nvdimm_info->dynamic_ipl_per_page_size);
		memcpy(apply_log_buffer + offset, apply_info.dynamic_start_pointer, nvdimm_info->dynamic_ipl_per_page_size);
		flush_cache(apply_log_buffer + offset, nvdimm_info->dynamic_ipl_per_page_size);
	}

	start_ptr = apply_log_buffer;
	end_ptr = apply_info.dynamic_start_pointer == NULL ? apply_log_buffer + nvdimm_info->static_ipl_per_page_size : apply_log_buffer + nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size;


	while(start_ptr < end_ptr){

		mlog_id_t log_type = mlog_id_t(mach_read_from_1(start_ptr));
		start_ptr +=1;
		ulint body_len = mach_read_from_2(start_ptr);
		start_ptr += 2;
		if(log_type != 41){
			continue;
		}else{
			ipl_log_to_rec = page+body_len;
		}
		start_ptr += body_len;

		if(rec == ipl_log_to_rec){
			apply_rec_cnt++;
			final_ipl_rec = ipl_log_to_rec;
			break;
		}
	}
	if(final_ipl_rec==NULL){
		*old_vers = NULL;
		return DB_SUCCESS;
	}
	if(apply_rec_cnt<2){
		return DB_FAIL;
	}

read_old_page:
	/* Read the old page from the disk */

	buf1 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	old_page = static_cast<byte*>(ut_align(buf1, UNIV_PAGE_SIZE));


	//fprintf(stderr, "Before locking space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);
	//mutex_enter(buf_page_get_mutex(bpage));
	//rw_lock_s_lock_gen(&((buf_block_t*) bpage)->lock,BUF_IO_READ);

	bpage = buf_page_init_for_read(&err, BUF_READ_ANY_PAGE, page_id, page_size, false);

	if (bpage == NULL) {
		return(DB_FAIL);
	}

	//fprintf(stderr, "Before fil_io space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);
	//thd_wait_begin(NULL, THD_WAIT_DISKIO);
	
	err = fil_io(IORequestRead, true, page_id, page_size, 0, page_size.physical(), 
			old_page, NULL);

	//rw_lock_s_unlock_gen(&((buf_block_t*) bpage)->lock,BUF_IO_READ);
	//mutex_exit(buf_page_get_mutex(bpage));		

	if(err!=DB_SUCCESS){
		bpage->io_fix==BUF_IO_NONE;
		fprintf(stderr, "failed to read an old page from the disk. space_id: %d page_no: %lu\n", page_id.space(), page_id.page_no());
		return DB_FAIL;	
	}

	//fprintf(stderr, "After fil_io space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);
	//fprintf(stderr, "After lock release space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);

	old_page_max_trx_id = page_get_max_trx_id(nvdimm_info->old_page);
	cur_page_max_trx_id = page_get_max_trx_id(page);

	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);


	/* 3. Traverse all the log records inside IPL region to find until which redo log we should apply based on trx_id
	 Apply the redo log and find compare the max_trx_id of the old bpage with the readview trx_id */

	start_ptr = apply_log_buffer;
	end_ptr = apply_info.dynamic_start_pointer == NULL ? apply_log_buffer + nvdimm_info->static_ipl_per_page_size : apply_log_buffer + nvdimm_info->static_ipl_per_page_size + nvdimm_info->dynamic_ipl_per_page_size;

	while(start_ptr < end_ptr){

		/* Copy the old page to temporary space */
		buf_frame_copy(temp_page, nvdimm_info->old_page);

		/* Get max_trx_id of the old page */
		temp_trx_id = old_page_max_trx_id;

		//fprintf(stderr, "temp_trx_id: %lu old_trx_id: %lu readview visible: %lu\n",page_get_max_trx_id(temp_page), old_page_max_trx_id, read_view->changes_visible(old_page_max_trx_id, clust_index->table->name));

		mlog_id_t log_type = mlog_id_t(mach_read_from_1(start_ptr));
		start_ptr +=1;
		ulint body_len = mach_read_from_2(start_ptr);
		start_ptr += 2;
		if(log_type == 0 && body_len == 0){
			cur_len = start_ptr - apply_log_buffer - 3;
			goto apply_end;
		}
		// Apply redo logs to the page
		old_page_max_trx_id = nvdimm_recv_parse_or_apply_log_rec_body(log_type, start_ptr, start_ptr + body_len, apply_info.block->page.id.space(), apply_info.block->page.id.page_no(), &temp_mtr, old_page, temp_trx_id);
		IPL_apply_cnt++;
		start_ptr += body_len;

		//page_header_print(old_page);
		//page_dir_print(old_page, 10);

		if(!read_view->changes_visible(old_page_max_trx_id, clust_index->table->name) && IPL_apply_cnt%2==0){
			//fprintf(stderr, "current version not visible, give temp_page\n");
			break;
		}
	}		
	cur_len = start_ptr - apply_log_buffer;

apply_end:
	cur_len += IPL_LOG_HEADER_SIZE;
	temp_mtr.discard_modifications();
	mtr_commit(&temp_mtr);

	if(start_ptr < end_ptr){  // all built version visible to the read trx -> provide old_page instead of temp_page
		buf_frame_copy(temp_page, old_page);
		temp_trx_id = old_page_max_trx_id;
	}

	//fprintf(stderr, "finished apply: space_id: %d page_no: %lu IPL_apply_cnt: %d bpage: %lu\n", page_id.space(), page_id.page_no(), IPL_apply_cnt, bpage);

	/* 4. After getting the right version of the IPL page, store the right record to the old_vers record */

get_rec_offset:
	if(resuse_prev_built_page==true){
		fprintf(stderr, "reuse_prev_built_page: %d bpage: %lu \n", resuse_prev_built_page, bpage);
		buf_frame_copy(temp_page, nvdimm_info->old_page);
	}

	*offsets = rec_get_offsets(
			rec, clust_index, *offsets, ULINT_UNDEFINED,
			offset_heap);

	//page_rec_print(rec, *offsets);


	if(*offsets==NULL){
		fprintf(stderr, "offset NULL during redo version creation\n");
		return DB_FAIL;
	}

	// cannot access old_vers -> need to

	const rec_t* temp_page_rec;
	ulint rec_slot_index = page_dir_find_owner_slot(rec);
	ulint heap_no = page_rec_get_heap_no(rec);

	temp_page_rec = page_find_rec_with_heap_no(temp_page, heap_no);

	//fprintf(stderr,"temp_page_rec: %lu rec: %lu\n", temp_page_resc, rec);

	ulint*		temp_offsets;
	mem_heap_t*	temp_offset_heap		= NULL;

	temp_offset_heap = mem_heap_create(1024);

	temp_offsets = rec_get_offsets(
			temp_page_rec, clust_index, temp_offsets, ULINT_UNDEFINED,
			&temp_offset_heap);

	if(temp_page_rec==NULL){
		return DB_FAIL;
		//return DB_FAIL;
	}else{
		//page_rec_print(temp_page_rec, *offsets);
	}

	byte* buf = static_cast<byte*>(
				mem_heap_alloc(
					in_heap, rec_offs_size(temp_offsets)));
	

	*old_vers = rec_copy(buf, temp_page_rec, *offsets); // copy old vers to buf
	rec_offs_make_valid(*old_vers, clust_index, *offsets);

	if(rec_get_deleted_flag(*old_vers, true)){
		return DB_FAIL;
	}

	if(resuse_prev_built_page==false){
		buf_frame_copy(nvdimm_info->old_page, temp_page);
	}

	if (vrow && *vrow) {
		*vrow = dtuple_copy(*vrow, in_heap);
		dtuple_dup_v_fld(*vrow, in_heap);
	}

	//fprintf(stderr, "End of func space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);



	return DB_SUCCESS;
}
