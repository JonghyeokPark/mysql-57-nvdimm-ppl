
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
#ifdef UNIV_NVDIMM_PPL
#include "nvdimm-ppl.h"
#include "mtr0log.h"
#include "page0page.h"
#include "buf0flu.h"

#include <emmintrin.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

std::vector<buf_page_t*> prebuilt_page_list;
buf_page_t * prebuilt_page_start_ptr = NULL;

bool alloc_first_ppl_to_bpage(buf_page_t * bpage){
	unsigned char * first_ppl_block_ptr = alloc_ppl_from_queue(normal_buf_pool_get(bpage->id));
	unsigned char temp_buf[10] = {0, };
	if(first_ppl_block_ptr == NULL) {
		set_normalize_flag(bpage, 3);
		return false;
	}
	ulint offset = 0;
	mach_write_to_1(((unsigned char *)temp_buf) + offset, 1); // Store First PPL Flag
	offset += 1;
	mach_write_to_1(((unsigned char *)temp_buf) + offset, 0); // Store Normalize Flag
	offset += 1;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.space()); // Store Space id
	offset += 4;
	mach_write_to_4(((unsigned char *)temp_buf) + offset, bpage->id.page_no());// Store Page_no
	offset += 4;

	//write to nvdimm
	memcpy_to_nvdimm(first_ppl_block_ptr, temp_buf, 10);
	//Set PPL Pointer 
	bpage->first_ppl_block_ptr = first_ppl_block_ptr;
	bpage->ppl_write_pointer = first_ppl_block_ptr + PPL_BLOCK_HDR_SIZE;
	bpage->block_used = PPL_BLOCK_HDR_SIZE;

	set_flag(&(bpage->flags), PPLIZED);
	return true;
}

bool alloc_nth_ppl_to_bpage(buf_page_t * bpage){
	unsigned char * new_ppl_block = alloc_ppl_from_queue(normal_buf_pool_get(bpage->id));
	if(new_ppl_block == NULL) return false;

	mach_write_to_1(new_ppl_block + NTH_PPL_BLOCK_MARKER, 0); // Store Nth PPL Flag
	flush_cache(new_ppl_block, 1);

	//현재 PPL의 다음 PPL을 가리키는 포인터를 현재 PPL에 기록
	mach_write_to_4(get_last_block_address_index(bpage), get_ppl_index_from_addr(nvdimm_info->ppl_start_pointer, new_ppl_block, nvdimm_info->each_ppl_size));
	flush_cache(get_last_block_address_index(bpage), 4);

	bpage->ppl_write_pointer = new_ppl_block + NTH_PPL_BLOCK_HEADER_SIZE;
	bpage->block_used = NTH_PPL_BLOCK_HEADER_SIZE;
	return true;
}

void copy_log_to_memory(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id){
	buf_block_t * block = (buf_block_t *)bpage;
	byte * write_pointer = block->in_memory_ppl_buf.open(len);
	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;
	//Step1. Write PPL header 
	mach_write_to_1(write_pointer, store_type); // mtr_log type
	write_pointer += 1;
	mach_write_to_2(write_pointer, log_body_size); //mtr_log body
	write_pointer += 2;
	mach_write_to_8(write_pointer, trx_id); // mtr_log trx_id
	write_pointer += 8;

	//Step2. Write PPL Payload
	memcpy(write_pointer, log, log_body_size);
	write_pointer += log_body_size;
	block->in_memory_ppl_buf.close(write_pointer);
}

void copy_log_to_ppl_directly(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id){
	unsigned char write_ipl_log_buffer [11] = {0, };
	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;

	//Step1. Write PPL header 
	mach_write_to_1(write_ipl_log_buffer, store_type); // mtr_log type
	mach_write_to_2(write_ipl_log_buffer + 1, log_body_size); //mtr_log body
	mach_write_to_8(write_ipl_log_buffer + 3, trx_id); // mtr_log trx_id

	//Step2. Write PPL Payload
	if(!copy_memory_log_to_ppl(write_ipl_log_buffer, APPLY_LOG_HDR_SIZE, bpage)) return;
	if(!copy_memory_log_to_ppl(log, log_body_size, bpage))	return;
	bpage->ppl_length += len;
	set_flag(&(bpage->flags), DIRECTLY_WRITE);
}

bool copy_memory_log_to_nvdimm(buf_page_t * bpage){
	buf_block_t * block = (buf_block_t *)bpage;
	mem_to_nvdimm_copy_t ppl_copy;
	ppl_copy.init(bpage);
	if(!block->in_memory_ppl_buf.for_each_block(ppl_copy)){ 
		return false;
	}
	bpage->ppl_length += block->in_memory_ppl_buf.size();
	set_page_lsn_and_length_in_ppl_header(bpage->first_ppl_block_ptr, bpage->newest_modification, block->in_memory_ppl_buf.size());
	insert_page_ppl_info_in_hash_table(bpage);
	return true;
}

bool copy_memory_log_to_ppl(unsigned char *log, ulint len, buf_page_t * bpage){
	uint left_length = nvdimm_info->each_ppl_size - bpage->block_used;
	while (len > left_length) {
		if(left_length == 0)	goto alloc_ppl;
		memcpy_to_nvdimm(bpage->ppl_write_pointer, log, left_length);

		len -= left_length;
		bpage->block_used += left_length;
		bpage->ppl_write_pointer += left_length;
		log += left_length;

alloc_ppl:
		if (!alloc_nth_ppl_to_bpage(bpage)) {
			set_normalize_flag(bpage, 3);
			return false;
		}
		left_length = nvdimm_info->each_ppl_size - bpage->block_used;
	}
	memcpy_to_nvdimm(bpage->ppl_write_pointer, log, len);

	bpage->block_used += len;
	bpage->ppl_write_pointer += len;
	return true;
}


void set_apply_info_and_log_apply(buf_block_t* block) {
	buf_page_t * apply_page = (buf_page_t *)block;
	mtr_t temp_mtr;
	ulint apply_log_size = get_ppl_length_from_ppl_header(apply_page);

	//Step 2. Apply log
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	all_ppl_apply_to_page(apply_page->first_ppl_block_ptr,apply_log_size, block, &temp_mtr);
	temp_mtr.discard_modifications();
	mtr_commit(&temp_mtr);

	//Step 3. Memory Return
}

void all_ppl_apply_to_page(byte *start_ptr, ulint apply_log_size, buf_block_t *block, mtr_t *temp_mtr) {
    byte *current_ptr = start_ptr + PPL_BLOCK_HDR_SIZE;
    byte *end_ptr = start_ptr + nvdimm_info->each_ppl_size;
    byte *next_ppl = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer, mach_read_from_4(start_ptr + PPL_HDR_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
    byte temp_buffer[400] ={0, }; // Temporary buffer to handle data that spans multiple segments
	mlog_id_t log_type;
	ulint log_body_length;
	trx_id_t trx_id;

    while (apply_log_size != 0) {
        if ((end_ptr - current_ptr) < APPLY_LOG_HDR_SIZE) {
            size_t remaining = end_ptr - current_ptr;
            memcpy(temp_buffer, current_ptr, remaining); // Copy the partial header to the temporary buffer
			apply_log_size -= remaining;
            current_ptr = fetch_next_segment(current_ptr, &end_ptr, &next_ppl);
            if (!current_ptr) {
                break; // If no more segments, exit loop
            }
            memcpy(temp_buffer + remaining, current_ptr, APPLY_LOG_HDR_SIZE - remaining);
            current_ptr += (APPLY_LOG_HDR_SIZE - remaining); // Update current_ptr beyond the header
			apply_log_size -= APPLY_LOG_HDR_SIZE - remaining;
            // Process the full header from temp_buffer
            log_type = (mlog_id_t)mach_read_from_1(temp_buffer);
            log_body_length = mach_read_from_2(temp_buffer + 1);
            trx_id = mach_read_from_8(temp_buffer + 3);

        }
		else{
			// Read log record header
			log_type = (mlog_id_t)mach_read_from_1(current_ptr);
			current_ptr += 1;
			apply_log_size -= 1;

			// Read log body length
			log_body_length = mach_read_from_2(current_ptr);
			current_ptr += 2;
			apply_log_size -= 2;

			// Read transaction ID
			trx_id = mach_read_from_8(current_ptr);
			current_ptr += 8;
			apply_log_size -= 8;
		}

		if ((end_ptr - current_ptr) < log_body_length) {
			size_t remaining = end_ptr - current_ptr;
			memcpy(temp_buffer, current_ptr, remaining);
			apply_log_size -= remaining;

			size_t copied_length = remaining;

			while (copied_length < log_body_length) {
				current_ptr = fetch_next_segment(current_ptr, &end_ptr, &next_ppl);

				if (!current_ptr) {
					// If there is no next segment, stop the loop
					fprintf(stderr, "Error: Unable to fetch the next segment. Incomplete log body read. Expected: %zu, Got: %zu\n", log_body_length, copied_length);
					return;
				}

				size_t segment_length = (log_body_length - copied_length <= end_ptr - current_ptr) ? (log_body_length - copied_length) : (end_ptr - current_ptr);
				memcpy(temp_buffer + copied_length, current_ptr, segment_length);
				copied_length += segment_length;
				current_ptr += segment_length;
				apply_log_size -= segment_length;
			}

			if (copied_length == log_body_length) {
				apply_log_record(log_type, temp_buffer, log_body_length, trx_id, block, temp_mtr);
			} else {
				// Exception handling: When not all segments are fetched
				fprintf(stderr, "Error: Incomplete log body read. Expected: %zu, Got: %zu\n", log_body_length, copied_length);
				break;
			}
		} else {
			apply_log_record(log_type, current_ptr, log_body_length, trx_id, block, temp_mtr);
			current_ptr += log_body_length; // Move past the log body
			apply_log_size -= log_body_length;
		}
    }
	block->page.ppl_write_pointer = current_ptr;
	block->page.block_used = current_ptr - (end_ptr - nvdimm_info->each_ppl_size);
}

// Helper function to handle log data that spans segment boundaries
byte* fetch_next_segment(byte* current_end, byte** new_end, byte** next_ppl) {
	if(next_ppl == NULL){
		fprintf(stderr, "Error : fetch_next_segment\n");
		return NULL;
	}
	*new_end = *next_ppl + nvdimm_info->each_ppl_size;
	byte * current_ptr = *next_ppl + NTH_PPL_BLOCK_HEADER_SIZE;
	byte * temp_ptr = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer, mach_read_from_4(*next_ppl + NTH_PPL_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
	*next_ppl = temp_ptr;
	return current_ptr;
}

void apply_log_record(mlog_id_t log_type, byte* log_data, uint length, trx_id_t trx_id, buf_block_t* block, mtr_t* temp_mtr) {
	if (nvdimm_recv_ipl_undo && ipl_active_trx_ids.find(trx_id) != ipl_active_trx_ids.end()) {
			// pass	
			ib::info() << "skip undo because this is created from trx which is active at the crash!";	
	} 
	else {
		recv_parse_or_apply_log_rec_body(
									log_type, log_data
									, log_data + length, block->page.id.space()
									, block->page.id.page_no(), block, temp_mtr);
	}
}

void insert_page_ppl_info_in_hash_table(buf_page_t * bpage){
	page_id_t page_id = bpage->id;
	std::pair <page_id_t, unsigned char *> insert_data = std::make_pair(bpage->id, bpage->first_ppl_block_ptr);
	buf_pool_t * buf_pool = normal_buf_pool_get(page_id);
	rw_lock_x_lock(&buf_pool->lookup_table_lock);
	buf_pool->ppl_look_up_table->insert(insert_data);
	rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	set_flag(&(bpage->flags), IN_LOOK_UP);
}

/* TODO Sjmun : 한 번도 Discard되지 않은 페이지들은 사실 IPL을 사용할 필요 없이 Global redo로그로만 복구가능한데..  */
void set_normalize_flag(buf_page_t * bpage, uint normalize_cause){
	if(bpage->normalize_cause == 0){
		bpage->normalize_cause = normalize_cause;
	}
	if(get_flag(&(bpage->flags), PPLIZED) && !get_flag(&(bpage->flags), NORMALIZE)){
		set_normalize_flag_in_ppl_header(bpage->first_ppl_block_ptr, 1);
	}
	set_flag(&(bpage->flags), NORMALIZE);
}

UNIV_INLINE
void 
check_normalize_cause(buf_page_t * bpage){
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		switch (bpage->normalize_cause)
		{
			case 0:
				// fprintf(stderr, "Normalize_cause,%f,Normal_write\n",(double)(time(NULL) - my_start));
				break;
			case 1:
				// fprintf(stderr, "Normalize_cause,%f,Record_movement\n",(double)(time(NULL) - my_start));
				break;
			case 2:
				// fprintf(stderr, "Normalize_cause,%f,Max_PPL_Size\n",(double)(time(NULL) - my_start));
				break;
			case 3:
				fprintf(stderr, "Normalize_cause,%f,PPL_Area_Lack\n",(double)(time(NULL) - my_start));
				break;
			case 4:
				// fprintf(stderr, "Normalize_cause,%f,Not_PPL_Target\n",(double)(time(NULL) - my_start));
				break;
			case 5:
				fprintf(stderr, "Normalize_cause,%f,Cleaning\n",(double)(time(NULL) - my_start));
				break;
			case 6:
				fprintf(stderr, "Normalize_cause,%f,LSN_GAP_Too_Large\n",(double)(time(NULL) - my_start));
				break;
			default:
				break;
		}
	}
	else{
		switch (bpage->normalize_cause)
		{
			case 0:
				// fprintf(stderr, "Normalize_cause,%f,Normal_write\n",(double)(time(NULL) - my_start));
				break;
			case 1:
				// fprintf(stderr, "Normalize_cause,%f,Direct_Record_movement\n",(double)(time(NULL) - my_start));
				break;
			case 2:
				// fprintf(stderr, "Normalize_cause,%f,Direct_Max_PPL_Size\n",(double)(time(NULL) - my_start));
				break;
			case 3:
				fprintf(stderr, "Normalize_cause,%f,Direct_PPL_Area_Lack\n",(double)(time(NULL) - my_start));
				break;
			case 4:
				// fprintf(stderr, "Normalize_cause,%f,Direct_Not_PPL_Target\n",(double)(time(NULL) - my_start));
				break;
			case 5:
				fprintf(stderr, "Normalize_cause,%f,Direct_Cleaning\n",(double)(time(NULL) - my_start));
				break;
			case 6:
				fprintf(stderr, "Normalize_cause,%f,Direct_LSN_GAP_Too_Large\n",(double)(time(NULL) - my_start));
				break;
			default:
				break;
		}
	}
}

void normalize_ppled_page(buf_page_t * bpage, page_id_t page_id){ 
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		buf_pool_t * buf_pool = normal_buf_pool_get(page_id);
		rw_lock_x_lock(&buf_pool->lookup_table_lock);
		buf_pool->ppl_look_up_table->erase(page_id);
		rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	}
	bpage->first_ppl_block_ptr = NULL;
	bpage->ppl_write_pointer = NULL;
	bpage->block_used = 0;
	bpage->ppl_length = 0;
	bpage->trx_id = 0;
	bpage->normalize_cause = 0;
	if(get_flag(&(bpage->flags), IN_PPL_BUF_POOL)){
		bpage->flags = 32;
	}
	else{
		bpage->flags = 0;
	}
	
}



void set_for_ppled_page(buf_page_t* bpage){
	bpage->trx_id = 0;
	bpage->first_ppl_block_ptr = NULL;
	bpage->ppl_write_pointer = NULL;
	bpage->block_used = 0;
	bpage->ppl_length = 0;
	if(!get_flag(&(bpage->flags), IN_PPL_BUF_POOL)){
		bpage->flags = 0;
	}
	bpage->normalize_cause = 0;
	page_id_t page_id = bpage->id;
	buf_pool_t * buf_pool = normal_buf_pool_get(page_id);
	rw_lock_s_lock(&buf_pool->lookup_table_lock);
	std::tr1::unordered_map<page_id_t, unsigned char * >::iterator it = buf_pool->ppl_look_up_table->find(page_id);
	rw_lock_s_unlock(&buf_pool->lookup_table_lock);
	if(it != buf_pool->ppl_look_up_table->end()){
		set_flag(&(bpage->flags), PPLIZED);
		set_flag(&(bpage->flags), IN_LOOK_UP);
		bpage->first_ppl_block_ptr = it->second;
		bpage->ppl_length = get_ppl_length_from_ppl_header(bpage);
	}
}

//Dynamic 영역을 가지고 있는 checkpoint page인지 확인하기.
bool check_can_be_skip(buf_page_t *bpage) {
    // 플래그 검사 및 빠른 반환 조건
	buf_pool_t * buf_pool = normal_buf_pool_get(bpage->id);
    if (get_flag(&(bpage->flags), NORMALIZE)) {
        return false;
    }

    if (get_flag(&(bpage->flags), DIRECTLY_WRITE)) {
		if(buf_pool->is_eager_normalize && get_ppl_length_from_ppl_header(bpage) > fourth_block_start_size){
			set_normalize_flag(bpage, 6);
			return false;
		}
        return true;
    }

    // in_memory_ppl_size 계산
    ulint in_memory_ppl_size = ((buf_block_t *)bpage)->in_memory_ppl_buf.size();

    if (in_memory_ppl_size == 0) {
        return false;
    }

    // PPLIZED 플래그에 따른 처리
    if (!get_flag(&(bpage->flags), PPLIZED)) {
        return true;
    }

    // 목표 길이 계산 및 크기 검사
    return false;
}


//Dynamic 영역을 가지고 있는 checkpoint page인지 확인하기.
bool check_can_be_pplized(buf_page_t *bpage) {
    if (get_flag(&(bpage->flags), NORMALIZE)) {
        return false;
    }

    if (get_flag(&(bpage->flags), DIRECTLY_WRITE)) {
        return true;
    }

    // in_memory_ppl_size 계산
    ulint in_memory_ppl_size = ((buf_block_t *)bpage)->in_memory_ppl_buf.size();

    if (in_memory_ppl_size == 0) {
        return false;
    }

    // PPLIZED 플래그에 따른 처리
    if (!get_flag(&(bpage->flags), PPLIZED)) {
        return alloc_first_ppl_to_bpage(bpage);
    }

    // 목표 길이 계산 및 크기 검사
    return false;
}


bool check_return_ppl_region(buf_page_t * bpage){
	if(!get_flag(&(bpage->flags), PPLIZED)){
		bpage->first_ppl_block_ptr = NULL;
		bpage->ppl_write_pointer = NULL;
		bpage->trx_id = 0;
		bpage->block_used = 0;
		bpage->normalize_cause = 0;
		bpage->ppl_length = 0;
		if(get_flag(&(bpage->flags), IN_PPL_BUF_POOL)){
			bpage->flags = 32;
		}
		else{
			bpage->flags = 0;
		}
	}
	else{
		if(get_flag(&(bpage->flags), NORMALIZE)){
			normalize_ppled_page(bpage, bpage->id);
			return true;
		}
	}
	return false;
}

unsigned char * get_last_block_address_index(buf_page_t * bpage){
	if(mach_read_from_4(bpage->first_ppl_block_ptr + PPL_HDR_DYNAMIC_INDEX) == 0){
		return bpage->first_ppl_block_ptr + PPL_HDR_DYNAMIC_INDEX;
	}
	return (bpage->ppl_write_pointer - bpage->block_used) + NTH_PPL_DYNAMIC_INDEX;
}

void set_ppl_length_in_ppl_header(buf_page_t * bpage, ulint length){
	unsigned char * first_ppl_block_ptr = bpage->first_ppl_block_ptr;
	mach_write_to_4(first_ppl_block_ptr + PPL_HDR_LEN, length);
	flush_cache(first_ppl_block_ptr + PPL_HDR_LEN, 4);
}

uint get_ppl_length_from_ppl_header(buf_page_t * bpage){
	return mach_read_from_4(bpage->first_ppl_block_ptr + PPL_HDR_LEN);
}

void set_page_lsn_in_ppl_header(unsigned char* first_ppl_block_ptr, lsn_t lsn){
  // (anonymous): recovery
	if (nvdimm_recv_running) return;
	mach_write_to_8(first_ppl_block_ptr + PPL_HDR_LSN, lsn);
	flush_cache(first_ppl_block_ptr + PPL_HDR_LSN, 8);
}

lsn_t get_page_lsn_from_ppl_header(unsigned char* first_ppl_block_ptr){
	return mach_read_from_8(first_ppl_block_ptr + PPL_HDR_LSN);
}

void set_normalize_flag_in_ppl_header(unsigned char * first_ppl_block_ptr, unsigned char value){
	mach_write_to_1(first_ppl_block_ptr + PPL_HDR_NORMALIZE_MARKER, value);
	flush_cache(first_ppl_block_ptr + PPL_HDR_NORMALIZE_MARKER, value);
}

unsigned char get_normalize_flag_in_ppl_header(unsigned char * first_ppl_block_ptr){
	return mach_read_from_1(first_ppl_block_ptr + PPL_HDR_NORMALIZE_MARKER);
}

unsigned char get_first_block_flag_in_ppl_header(unsigned char * first_ppl_block_ptr){
	return mach_read_from_1(first_ppl_block_ptr + PPL_HDR_FIRST_MARKER);
}

void set_page_lsn_and_length_in_ppl_header(unsigned char* first_ppl_block_ptr, lsn_t lsn, ulint length){
  // (anonymous): recovery
	if (nvdimm_recv_running) return;
	// Set LSN and Length simultaneously making one block
	unsigned char write_ipl_log_buffer [12] = {0, };
	mach_write_to_4(write_ipl_log_buffer, length);
	mach_write_to_8(write_ipl_log_buffer + 4, lsn);
	memcpy_to_nvdimm(first_ppl_block_ptr + PPL_HDR_LEN, write_ipl_log_buffer, 12);
	flush_cache(first_ppl_block_ptr + PPL_HDR_LEN, 12);
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

void memcpy_to_nvdimm(void *dest, void *src, size_t size){
	memcpy(dest, src, size);
	flush_cache(dest, size);
	
}

void memset_to_nvdimm(void* dest, int value, size_t size){
	memset(dest, value, size);	
	flush_cache(dest, size);
}

bool
can_page_be_pplized(
/*==========================*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr/*!< in: buffer end */
)
{
	mlog_id_t type;
	ulint space, page_no;
	if (end_ptr < ptr + 1) {

		return false;
	}

	type = (mlog_id_t)((ulint)*ptr & ~MLOG_SINGLE_REC_FLAG);
	ut_ad(type <= MLOG_BIGGEST_TYPE);

	ptr++;

	if (end_ptr < ptr + 2) {

		return false;
	}

	space = mach_parse_compressed(&ptr, end_ptr);

	if (ptr != NULL) {
		page_no = mach_parse_compressed(&ptr, end_ptr);
	}
	const page_id_t	page_id(space, page_no);
	buf_pool_t * buf_pool = buf_pool_get(page_id);
	buf_page_t * buf_page = buf_page_get_also_watch(buf_pool, page_id);
	if(!is_system_or_undo_tablespace(space) && 
		!get_flag(&(buf_page->flags), NORMALIZE) && 
		page_is_leaf(((buf_block_t *)buf_page)->frame) && 
		buf_page_in_file(buf_page) &&
		page_id.page_no() > 7){
		return true;
	}
	set_normalize_flag(buf_page, 4);
	return false;
}


void 
init_prebuilt_page_cache(std::vector<buf_page_t*> prebuilt_page_list){
    prebuilt_page_list.clear();
}

buf_page_t* 
add_prebuilt_page(buf_page_t* bpage){

    byte * buf = NULL;
    buf_page_t* prebuilt_page = NULL;


    buf = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
    prebuilt_page = static_cast<buf_page_t*>(ut_align(buf, UNIV_PAGE_SIZE));

    // copy current version to  prebuilt_page
    memcpy(prebuilt_page, bpage, UNIV_PAGE_SIZE);
    buf_block_t* block = buf_page_get_block(prebuilt_page); 

    // apply all the ipl log inside nvdimm 
    set_apply_info_and_log_apply(block);

    // add prebuilt page to the list
    prebuilt_page_list.push_back(prebuilt_page);

    // set start_ptr 
    if(prebuilt_page_list.size()==1){
        prebuilt_page_start_ptr=prebuilt_page;
    }
    return prebuilt_page;
}


void 
remove_prebuilt_page_from_list(buf_page_t* prebuilt_page, std::vector<buf_page_t*> prebuilt_page_list){  
    for(int idx=0; idx<prebuilt_page_list.size(); idx++){
        if(prebuilt_page_list[idx]==prebuilt_page){
            prebuilt_page_list.erase(prebuilt_page_list.begin()+idx);
        }
    }
}

buf_page_t* 
find_prebuilt_page_from_list(buf_page_t* prebuilt_page, std::vector<buf_page_t*> prebuilt_page_list){
    int idx = 0;
    int found_cnt = 0;
    int found_idx = 0;

    for(idx=0; idx<prebuilt_page_list.size(); idx++){
        if(prebuilt_page_list[idx]==prebuilt_page){
            found_idx = idx;
            found_cnt++;
        }
    }
    if(found_idx>1){
        fprintf(stderr, "MVCC WARNING: there are multiple prebuilt page for this pid...\n");
    }
    if(idx==prebuilt_page_list.size()){
        return NULL;
    }else{
        return prebuilt_page_list[idx];
    }
}

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


	
	// apply_log_info apply_info;
	// apply_info.static_start_pointer = bpage->static_ipl_pointer;
	// apply_info.dynamic_start_pointer = get_dynamic_ipl_pointer(bpage);
	// apply_info.block = block;

	/* 1. Read the old data page from the disk for page-level version build */

	page_t* page = block->frame;
	UNIV_MEM_ASSERT_RW(page, UNIV_PAGE_SIZE);
	ulint id = dict_index_get_space(clust_index);
	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(id, &found));

	byte*	buf2;
	byte*	temp_page;
	dberr_t		err = DB_SUCCESS;
	buf_block_t* temp_block;
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
	buf_page_t* temp_bpage;
	ulint apply_log_size;


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

	apply_log_size = get_ppl_length_from_ppl_header(bpage);
	start_ptr = bpage->first_ppl_block_ptr;
	byte *current_ptr = start_ptr + PPL_BLOCK_HDR_SIZE;
    end_ptr = start_ptr + nvdimm_info->each_ppl_size;
    byte *next_ppl = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer, mach_read_from_4(start_ptr + PPL_HDR_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
    byte temp_buffer[400] ={0, }; // Temporary buffer to handle data that spans multiple segments
	mlog_id_t log_type;
	ulint log_body_length;
	trx_id_t trx_id;


	buf2 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	temp_page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));


	if(nvdimm_info->old_page==NULL){
		nvdimm_info->old_page = (byte *)calloc(UNIV_PAGE_SIZE, sizeof(char));
		buf_frame_copy(nvdimm_info->old_page, page);
	}


	// same page built right before -> reuse it
	if(page_get_page_no(nvdimm_info->old_page)==page_get_page_no(page) && temp_old_page_ptr!=NULL){
		resuse_prev_built_page = true;
		goto get_rec_offset;
	}

	// check prebuilt page list
	temp_bpage = find_prebuilt_page_from_list(bpage, prebuilt_page_list);
	if(temp_bpage!=NULL){
		memcpy(bpage, temp_bpage, UNIV_PAGE_SIZE);
		remove_prebuilt_page_from_list(temp_bpage, prebuilt_page_list);
		fprintf(stderr, "prebuilt page list size: %d\n", prebuilt_page_list.size());
	}
	
	/* 1. Copy PPL region to memory */

	if(final_ipl_rec==NULL){
		*old_vers = NULL;
		return DB_SUCCESS;
	}
	if(apply_log_size<64){
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
	 
	 while (apply_log_size != 0 && read_view->changes_visible(old_page_max_trx_id, clust_index->table->name)  ) {
		/* Copy the old page to temporary space */
		buf_frame_copy(temp_page, nvdimm_info->old_page);

		temp_trx_id = old_page_max_trx_id;

        if ((end_ptr - current_ptr) < APPLY_LOG_HDR_SIZE) {
            size_t remaining = end_ptr - current_ptr;
            memcpy(temp_buffer, current_ptr, remaining); // Copy the partial header to the temporary buffer
			apply_log_size -= remaining;
            current_ptr = fetch_next_segment(current_ptr, &end_ptr, &next_ppl);
            if (!current_ptr) {
                break; // If no more segments, exit loop
            }
            memcpy(temp_buffer + remaining, current_ptr, APPLY_LOG_HDR_SIZE - remaining);
            current_ptr += (APPLY_LOG_HDR_SIZE - remaining); // Update current_ptr beyond the header
			apply_log_size -= APPLY_LOG_HDR_SIZE - remaining;
            // Process the full header from temp_buffer
            log_type = (mlog_id_t)mach_read_from_1(temp_buffer);
            log_body_length = mach_read_from_2(temp_buffer + 1);
            trx_id = mach_read_from_8(temp_buffer + 3);

        }else{
			// Read log record header
			log_type = (mlog_id_t)mach_read_from_1(current_ptr);
			current_ptr += 1;
			apply_log_size -= 1;

			// Read log body length
			log_body_length = mach_read_from_2(current_ptr);
			current_ptr += 2;
			apply_log_size -= 2;

			// Read transaction ID
			trx_id = mach_read_from_8(current_ptr);
			current_ptr += 8;
			apply_log_size -= 8;
		}

		if ((end_ptr - current_ptr) < log_body_length) {
			size_t remaining = end_ptr - current_ptr;
			memcpy(temp_buffer, current_ptr, remaining);
			apply_log_size -= remaining;

			size_t copied_length = remaining;

			while (copied_length < log_body_length) {
				current_ptr = fetch_next_segment(current_ptr, &end_ptr, &next_ppl);

				if (!current_ptr) {
					// If there is no next segment, stop the loop
					fprintf(stderr, "Error: Unable to fetch the next segment. Incomplete log body read. Expected: %zu, Got: %zu\n", log_body_length, copied_length);
					return DB_ERROR;
				}

				size_t segment_length = (log_body_length - copied_length <= end_ptr - current_ptr) ? (log_body_length - copied_length) : (end_ptr - current_ptr);
				memcpy(temp_buffer + copied_length, current_ptr, segment_length);
				copied_length += segment_length;
				current_ptr += segment_length;
				apply_log_size -= segment_length;
			}

			if (copied_length == log_body_length) {
				old_page_max_trx_id = nvdimm_recv_parse_or_apply_log_rec_body(log_type, current_ptr, current_ptr + log_body_length, block->page.id.space(), block->page.id.page_no(), &temp_mtr, old_page, temp_trx_id);
				IPL_apply_cnt++;
			} else {
				// Exception handling: When not all segments are fetched
				fprintf(stderr, "Error: Incomplete log body read. Expected: %zu, Got: %zu\n", log_body_length, copied_length);
				break;
			}
		} else {
			old_page_max_trx_id = nvdimm_recv_parse_or_apply_log_rec_body(log_type, current_ptr, current_ptr + log_body_length, block->page.id.space(), block->page.id.page_no(), &temp_mtr, old_page, temp_trx_id);
			IPL_apply_cnt++;
			current_ptr += log_body_length; // Move past the log body
			apply_log_size -= log_body_length;
		}
		old_page_max_trx_id = page_get_max_trx_id(nvdimm_info->old_page);
    }
	
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
#endif
