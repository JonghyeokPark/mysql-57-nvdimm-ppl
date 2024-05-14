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
#ifdef UNIV_NVDIMM_IPL
#include "nvdimm-ipl.h"
#include "mtr0log.h"
#include "page0page.h"
#include "buf0flu.h"
//싹다 클래스로 캡슐레이션을 해버려야 되나...
// 나중에 시간되면 해보기.

bool alloc_cxl_write_pointer_to_bpage(buf_page_t * bpage){
	unsigned char * static_ipl_pointer = alloc_ppl_from_queue(buf_pool_get(bpage->id));
	unsigned char temp_buf[8] = {NULL, };
	if(static_ipl_pointer == NULL) return false;
	ulint offset = 0;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.space()); // Store Space id
	offset += 4;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.page_no());// Store Page_no
	offset += 4;

	//nvdimm에 작성
	memcpy_to_cxl(static_ipl_pointer, temp_buf, 8);
	//IPL Pointer 설정
	bpage->static_ipl_pointer = static_ipl_pointer;
	bpage->ipl_write_pointer = static_ipl_pointer + IPL_HDR_SIZE;

	set_flag(&(bpage->flags), PPLIZED);
	// fprintf(stderr, "alloc static ipl (%u, %u) static_ipl: %p now_write_pointer: %p\n", bpage->id.space(), bpage->id.page_no(),bpage->static_ipl_pointer ,bpage->ipl_write_pointer);
	return true;
}

/* Sjmun IPL Log를 쓸때의 핵심*/
/* SIPL 영역 공간이 부족한 경우는 2개로 나누어서 기존 IPL 영역, 새로운 IPL 영역에 작성하게 됨*/
/* Log write atomicity를 보장하기 위해서는 두번째 파트를 다 작성하고 Length를 Flush cache*/

void nvdimm_ipl_add(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id){
	buf_block_t * block = (buf_block_t *)bpage;
	byte * write_pointer = block->in_memory_ppl_buf.open(len);
	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;
	//Step1. Apply log header 작성
	mach_write_to_1(write_pointer, store_type); // mtr_log type
	write_pointer += 1;
	mach_write_to_2(write_pointer, log_body_size); //mtr_log body
	write_pointer += 2;
	mach_write_to_8(write_pointer, trx_id); // mtr_log trx_id
	write_pointer += 8;

	//Step2. Apply log body 작성
	memcpy(write_pointer, log, log_body_size);
	write_pointer += log_body_size;
	block->in_memory_ppl_buf.close(write_pointer);
	// fprintf(stderr, "After nvdimm_ipl_add(%u, %u) Log type: %d, log_len: %lu, pointer: %p, Size: %lu\n", bpage->id.space(), bpage->id.page_no(), type, len, write_pointer, block->in_memory_ppl_buf.size());
}

void copy_memory_log_to_cxl(buf_page_t * bpage){
	buf_block_t * block = (buf_block_t *)bpage;
	mem_to_cxl_copy_t cxl_copy;
	ulint mem_log_size = block->in_memory_ppl_buf.size();
	// fprintf(stderr, "(%u, %u), copy_memory_log_to_cxl, CXL pointer %p, Write Pointer: %p size: %lu \n", bpage->id.space(), bpage->id.page_no(), bpage->static_ipl_pointer, bpage->ipl_write_pointer, mem_log_size);
	cxl_copy.init(bpage->ipl_write_pointer);
	block->in_memory_ppl_buf.for_each_block(cxl_copy);
	set_ipl_length_in_ipl_header(bpage, mem_log_size + get_ipl_length_from_in_cxl(bpage));
	bpage->ipl_write_pointer += mem_log_size;
	insert_page_ipl_info_in_hash_table(bpage);
}


void set_apply_info_and_log_apply(buf_block_t* block) {
	buf_page_t * apply_page = (buf_page_t *)block;
	mtr_t temp_mtr;
	unsigned char * apply_log_buf;
	ulint apply_log_size;

	//Step 1. CXL 영역에 있는 log를 메모리로 복사
	apply_log_size = get_ipl_length_from_in_cxl(apply_page) - IPL_HDR_SIZE;
	apply_log_buf = (unsigned char *)calloc(apply_log_size, sizeof(unsigned char));
	memcpy_from_cxl(apply_log_buf, apply_page->static_ipl_pointer + IPL_HDR_SIZE, apply_log_size);
	// fprintf(stderr, "(%u, %u), copy_memory_log_to_cxl, CXL pointer %p, Write Pointer: %p size: %lu \n", apply_page->id.space(), apply_page->id.page_no(), apply_page->static_ipl_pointer, apply_page->ipl_write_pointer, apply_log_size);

	//Step 2. Apply log
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	ipl_log_apply(apply_log_buf, apply_log_buf + apply_log_size, block, &temp_mtr);
	temp_mtr.discard_modifications();
	mtr_commit(&temp_mtr);

	//Step 3. Memory Return
	free(apply_log_buf);
}

void ipl_log_apply(byte * start_ptr, byte * end_ptr, buf_block_t * block, mtr_t * temp_mtr){
	// fprintf(stderr,"Apply! (%u, %u) start_ptr: %p, end_ptr: %p\n",block->page.id.space(), block->page.id.page_no(), start_ptr, end_ptr);
	byte * apply_ptr = start_ptr;
	page_id_t page_id = block->page.id;
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

		// (jhpark): ipl-undo 
		//check trx-id and skip
		if (nvdimm_recv_ipl_undo && ipl_active_trx_ids.find(trx_id) != ipl_active_trx_ids.end()) {
			// pass	
			ib::info() << "skip undo because this is created from trx which is active at the crash!";	
		} else {
			recv_parse_or_apply_log_rec_body(
										log_type, apply_ptr
										, apply_ptr + body_len, page_id.space()
										, page_id.page_no(), block, temp_mtr);
			apply_ptr += body_len;
		}
		// fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu, apply_ptr: %p, start_ptr: %p\n",page_id.space(), page_id.page_no(), log_type, body_len, apply_ptr, start_ptr);
	} // end-of-while
}


void insert_page_ipl_info_in_hash_table(buf_page_t * bpage){
	page_id_t page_id = bpage->id;
	std::pair <page_id_t, unsigned char *> insert_data = std::make_pair(bpage->id, bpage->static_ipl_pointer);
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	rw_lock_x_lock(&buf_pool->lookup_table_lock);
	buf_pool->ipl_look_up_table->insert(insert_data);
	rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	set_flag(&(bpage->flags), IN_LOOK_UP);
}

/* TODO Sjmun : 한 번도 Discard되지 않은 페이지들은 사실 IPL을 사용할 필요 없이 Global redo로그로만 복구가능한데..  */
void set_noramlize_flag(buf_page_t * bpage){
	set_flag(&(bpage->flags), NORMALIZE);
}

/* Unset_flag를 해주지 않아도 Static_ipl이 free 되면 초기화 됨*/
void normalize_ipl_page(buf_page_t * bpage, page_id_t page_id){ 
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		rw_lock_x_lock(&buf_pool->lookup_table_lock);
		buf_pool->ipl_look_up_table->erase(page_id);
		rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	}
	bpage->static_ipl_pointer = NULL;
	bpage->ipl_write_pointer = NULL;
	bpage->flags = 0;
	bpage->trx_id = 0;
}



void set_for_ipl_page(buf_page_t* bpage){
	bpage->trx_id = 0;
	bpage->static_ipl_pointer = NULL;
	bpage->flags = 0;
	bpage->ipl_write_pointer = NULL;
	page_id_t page_id = bpage->id;
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	rw_lock_s_lock(&buf_pool->lookup_table_lock);
	std::tr1::unordered_map<page_id_t, unsigned char * >::iterator it = buf_pool->ipl_look_up_table->find(page_id);
	rw_lock_s_unlock(&buf_pool->lookup_table_lock);
	if(it != buf_pool->ipl_look_up_table->end()){
		set_flag(&(bpage->flags), PPLIZED);
		set_flag(&(bpage->flags), IN_LOOK_UP);
		bpage->static_ipl_pointer = it->second;
		bpage->ipl_write_pointer = it->second + get_ipl_length_from_ipl_header(bpage);
	}
	
}


//Dynamic 영역을 가지고 있는 checkpoint page인지 확인하기.
bool check_can_be_pplized(buf_page_t * bpage){
	buf_block_t * block = (buf_block_t *)bpage;
	ulint in_memory_log_size = block->in_memory_ppl_buf.size();
	if(!get_flag(&(bpage->flags), NORMALIZE) && in_memory_log_size != 0 && in_memory_log_size <= nvdimm_info->each_ppl_size - IPL_HDR_SIZE){
		if(!get_flag(&(bpage->flags), PPLIZED)){
			//1. PPLIZED 되지 않은 페이지인 경우 CXL 영역 할당
			return alloc_cxl_write_pointer_to_bpage(bpage);
		}
		if(in_memory_log_size > get_left_size_in_cxl(bpage)) goto normal_page_case;
		return true;
	}
normal_page_case:
	set_noramlize_flag(bpage);
	return false;
}

bool check_return_ppl_region(buf_page_t * bpage){
	if(get_flag(&(bpage->flags), PPLIZED) == false){
		bpage->static_ipl_pointer = NULL;
		bpage->ipl_write_pointer = NULL;
		bpage->flags = 0;
		bpage->trx_id = 0;
		return false;
	}
	else{
		if(get_flag(&(bpage->flags), NORMALIZE)){
			normalize_ipl_page(bpage, bpage->id);
			return true;
		}
	}
	return false;
}


ulint get_ipl_length_from_in_cxl(buf_page_t * bpage){
	return bpage->ipl_write_pointer - bpage->static_ipl_pointer;
}

ulint get_left_size_in_cxl(buf_page_t * bpage){
	return nvdimm_info->each_ppl_size - get_ipl_length_from_in_cxl(bpage);
}

void set_ipl_length_in_ipl_header(buf_page_t * bpage, ulint length){
	unsigned char * static_ipl_pointer = bpage->static_ipl_pointer;
	mach_write_to_4(static_ipl_pointer + IPL_HDR_LEN, length);
	flush_cache(static_ipl_pointer + IPL_HDR_LEN, 4);
}

uint get_ipl_length_from_ipl_header(buf_page_t * bpage){
	return mach_read_from_4(bpage->static_ipl_pointer + IPL_HDR_LEN);
}

void set_page_lsn_in_ipl_header(unsigned char* static_ipl_pointer, lsn_t lsn){
  // (jhpark): recovery
  if (nvdimm_recv_running) return;
	mach_write_to_8(static_ipl_pointer + IPL_HDR_LSN, lsn);
	flush_cache(static_ipl_pointer + IPL_HDR_LSN, 8);
}

lsn_t get_page_lsn_from_ipl_header(unsigned char* static_ipl_pointer){
	return mach_read_from_8(static_ipl_pointer + IPL_HDR_LSN);
}

void set_normalize_flag_in_ipl_header(unsigned char * static_ipl_pointer){
	set_flag(static_ipl_pointer + IPL_HDR_FLAG, NORMALIZE);
	flush_cache(static_ipl_pointer + IPL_HDR_FLAG, 1);
}

unsigned char * get_flag_in_ipl_header(unsigned char * static_ipl_pointer){
	return static_ipl_pointer + IPL_HDR_FLAG;
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

void memcpy_to_cxl(void *dest, void *src, size_t size){
	memcpy(dest, src, size);
	flush_cache(dest, size);
}

void memcpy_from_cxl(void *dest, void *src, size_t size){
	memcpy(dest, src, size);
	flush_cache(dest, size);
}

void memset_to_cxl(void* dest, int value, size_t size){
	memset(dest, value, size);	
	flush_cache(dest, size);
}
#endif