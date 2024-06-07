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

#include <emmintrin.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

bool alloc_first_ppl_to_bpage(buf_page_t * bpage){
	unsigned char * static_ipl_pointer = alloc_ppl_from_queue(normal_buf_pool_get(bpage->id));
	unsigned char temp_buf[8] = {0};
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
	bpage->block_used = IPL_HDR_SIZE;

	set_flag(&(bpage->flags), PPLIZED);
	// set_ipl_length_in_ipl_header(bpage, 0);
	// fprintf(stderr, "Alloc First PPL (%u, %u) New Block: %p Write Pointer: %p\n", bpage->id.space(), bpage->id.page_no(), static_ipl_pointer, bpage->ipl_write_pointer);
	return true;
}

bool alloc_nth_ppl_to_bpage(buf_page_t * bpage){
	unsigned char * new_ppl_block = alloc_ppl_from_queue(normal_buf_pool_get(bpage->id));
	if(new_ppl_block == NULL) return false;
	mach_write_to_4(get_last_block_address_index(bpage), get_ipl_index_from_addr(nvdimm_info->static_start_pointer, new_ppl_block, nvdimm_info->each_ppl_size));
	
	bpage->ipl_write_pointer = new_ppl_block + NTH_IPL_HEADER_SIZE;
	bpage->block_used = 4;
	// fprintf(stderr, "Alloc Nth PPL (%u, %u) New Block pointer: %p, New Block index: %u, Write Pointer: %p\n", 
	// 		bpage->id.space(), 
	// 		bpage->id.page_no(), 
	// 		new_ppl_block,
	// 		get_ipl_index_from_addr(nvdimm_info->static_start_pointer, new_ppl_block, nvdimm_info->each_ppl_size),
	// 		bpage->ipl_write_pointer);
	return true;
}

/* Sjmun IPL Log를 쓸때의 핵심*/
/* SIPL 영역 공간이 부족한 경우는 2개로 나누어서 기존 IPL 영역, 새로운 IPL 영역에 작성하게 됨*/
/* Log write atomicity를 보장하기 위해서는 두번째 파트를 다 작성하고 Length를 Flush cache*/

// void copy_log_to_memory(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id){
// 	buf_block_t * block = (buf_block_t *)bpage;
// 	byte * write_pointer = block->in_memory_ppl_buf.open(len);
// 	unsigned char store_type = type;
// 	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;
// 	//Step1. Apply log header 작성
// 	mach_write_to_1(write_pointer, store_type); // mtr_log type
// 	write_pointer += 1;
// 	mach_write_to_2(write_pointer, log_body_size); //mtr_log body
// 	write_pointer += 2;
// 	mach_write_to_8(write_pointer, trx_id); // mtr_log trx_id
// 	write_pointer += 8;

// 	//Step2. Apply log body 작성
// 	memcpy(write_pointer, log, log_body_size);
// 	write_pointer += log_body_size;
// 	block->in_memory_ppl_buf.close(write_pointer);
// 	// fprintf(stderr, "After copy_log_to_memory(%u, %u) Log type: %d, log_len: %lu, pointer: %p, Size: %lu\n", bpage->id.space(), bpage->id.page_no(), type, len - 11, write_pointer, block->in_memory_ppl_buf.size());
// }

void copy_log_to_ppl_directly(unsigned char *log, ulint len, mlog_id_t type, buf_page_t * bpage, trx_id_t trx_id){
	unsigned char write_ipl_log_buffer [11] = {0, };
	unsigned char store_type = type;
	unsigned short log_body_size = len - APPLY_LOG_HDR_SIZE;

	//Step1. Apply log header 작성
	mach_write_to_1(write_ipl_log_buffer, store_type); // mtr_log type
	mach_write_to_2(write_ipl_log_buffer + 1, log_body_size); //mtr_log body
	mach_write_to_8(write_ipl_log_buffer + 3, trx_id); // mtr_log trx_id

	//Step2. Apply log body 작성
	if(!copy_memory_log_to_ppl(write_ipl_log_buffer, APPLY_LOG_HDR_SIZE, bpage)) return;
	if(!copy_memory_log_to_ppl(log, log_body_size, bpage))	return;

	set_ipl_length_in_ipl_header(bpage, len + get_ipl_length_from_ipl_header(bpage));
	// fprintf(stderr, "After copy_log_to_ppl_directly(%u, %u) Log type: %d, log_len: %lu, pointer: %p, Size: %lu\n", bpage->id.space(), bpage->id.page_no(), type, len - 11, bpage->ipl_write_pointer, bpage->block_used);
}

// bool copy_memory_log_to_cxl(buf_page_t * bpage){
// 	buf_block_t * block = (buf_block_t *)bpage;
// 	mem_to_cxl_copy_t cxl_copy;
// 	// fprintf(stderr, "(%u, %u), copy_memory_log_to_cxl, CXL pointer %p, Write Pointer: %p size: %lu \n", bpage->id.space(), bpage->id.page_no(), bpage->static_ipl_pointer, bpage->ipl_write_pointer, block->in_memory_ppl_buf.size());
// 	cxl_copy.init(bpage);
// 	if(!block->in_memory_ppl_buf.for_each_block(cxl_copy)){ // PPL 영역이 부족해서, copy를 못한 경우.
// 		// fprintf(stderr, "Error : copy_memory_log_to_cxl\n");
// 		return false;
// 	}
// 	set_ipl_length_in_ipl_header(bpage, block->in_memory_ppl_buf.size() + get_ipl_length_from_ipl_header(bpage));
// 	insert_page_ipl_info_in_hash_table(bpage);
// 	return true;
// }

bool copy_memory_log_to_ppl(unsigned char *log, ulint len, buf_page_t * bpage){
	uint left_length = nvdimm_info->each_ppl_size - bpage->block_used;
	// fprintf(stderr, "Start Copy Log to PPL (%u, %u) PPL Pointer: %p, Log Pointer: %p, Log Length: %lu Left Length: %lu\n", bpage->id.space(), bpage->id.page_no(),
	// 		bpage->ipl_write_pointer,
	// 		log,
	// 		len,
	// 		left_length);
	// len을 쓸 때까지 while문을 들면서 반복
	while (len > left_length) {
		if(left_length == 0)	goto alloc_ppl;
		// 1. 현재 PPL에 남은 공간을 채우고
		memcpy_to_cxl(bpage->ipl_write_pointer, log, left_length);
		// 2. PPL에 쓴 길이만큼 빼주고
		len -= left_length;
		// 3. PPL에 쓴 길이만큼 블록 사용량을 늘려주고
		bpage->block_used += left_length;
		bpage->ipl_write_pointer += left_length;
		// 4. PPL에 쓴 길이만큼 log를 빼주고
		log += left_length;
		// 5. 다음 PPL을 할당받아서
		// fprintf(stderr, "More Copy Log to PPL (%u, %u) PPL Pointer: %p, Log Pointer: %p, Size: %lu\n", bpage->id.space(), bpage->id.page_no(), bpage->ipl_write_pointer, log, left_length);

alloc_ppl:
		if (!alloc_nth_ppl_to_bpage(bpage)) {
			// 6. 할당받지 못하면 에러, normalize flag
			// fprintf(stderr, "Error : alloc_nth_ppl_to_bpage\n");
			set_normalize_flag(bpage, 3);
			return false;
		}
		// 7. 남은 길이를 다시 계산
		left_length = nvdimm_info->each_ppl_size - bpage->block_used;
	}
	// 남은 길이만큼 쓰고
	memcpy_to_cxl(bpage->ipl_write_pointer, log, len);
	// 쓴 길이만큼 블록 사용량을 늘려주고
	bpage->block_used += len;
	bpage->ipl_write_pointer += len;
	// fprintf(stderr, "Enough Copy Log to PPL (%u, %u) PPL Pointer: %p, Log Pointer: %p, Size: %lu\n", bpage->id.space(), bpage->id.page_no(), bpage->ipl_write_pointer, log, len);
	return true;
}


void set_apply_info_and_log_apply(buf_block_t* block) {
	buf_page_t * apply_page = (buf_page_t *)block;
	mtr_t temp_mtr;
	ulint apply_log_size = get_ipl_length_from_ipl_header(apply_page);
	// fprintf(stderr, "(%u, %u), copy_memory_log_to_cxl, CXL pointer %p, size: %lu \n",
	// 		apply_page->id.space(), 
	// 		apply_page->id.page_no(), 
	// 		apply_page->static_ipl_pointer, 
	// 		apply_log_size);

	//Step 2. Apply log
	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);
	ipl_log_apply(apply_page->static_ipl_pointer,apply_log_size, block, &temp_mtr);
	temp_mtr.discard_modifications();
	mtr_commit(&temp_mtr);

	//Step 3. Memory Return
}

void ipl_log_apply(byte *start_ptr, ulint apply_log_size, buf_block_t *block, mtr_t *temp_mtr) {
    byte *current_ptr = start_ptr + IPL_HDR_SIZE;
    byte *end_ptr = start_ptr + nvdimm_info->each_ppl_size;
    byte *next_ppl = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, mach_read_from_4(start_ptr + IPL_HDR_DYNAMIC_INDEX), nvdimm_info->each_ppl_size);
    byte temp_buffer[2048]; // Temporary buffer to handle data that spans multiple segments
	mlog_id_t log_type;
	ulint log_body_length;
	trx_id_t trx_id;

    while (apply_log_size != 0) {
        if ((end_ptr - current_ptr) < APPLY_LOG_HDR_SIZE) {
            size_t remaining = end_ptr - current_ptr;
            memcpy(temp_buffer, current_ptr, remaining); // Copy the partial header to the temporary buffer
			apply_log_size -= remaining;
            current_ptr = fetch_next_segment(current_ptr, &end_ptr, &next_ppl);
			// fprintf(stderr, "(%u, %u) No Header: current_end : %p, new_end : %p, next_ppl : %p\n",block->page.id.space(), block->page.id.page_no(), current_ptr, end_ptr, next_ppl);
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
				// fprintf(stderr, "(%u, %u) No Body: current_end : %p, new_end : %p, next_ppl : %p\n",
						// block->page.id.space(), block->page.id.page_no(), current_ptr, end_ptr, next_ppl);

				if (!current_ptr) {
					// 다음 세그먼트가 없으면 루프를 중단
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
				// 예외 처리: 모든 세그먼트를 가져오지 못했을 때의 처리
				fprintf(stderr, "Error: Incomplete log body read. Expected: %zu, Got: %zu\n", log_body_length, copied_length);
				break;
			}
		} else {
			apply_log_record(log_type, current_ptr, log_body_length, trx_id, block, temp_mtr);
			current_ptr += log_body_length; // Move past the log body
			apply_log_size -= log_body_length;
		}
    }
	block->page.ipl_write_pointer = current_ptr;
	block->page.block_used = current_ptr - (end_ptr - nvdimm_info->each_ppl_size);

	// fprintf(stderr, "After ipl_log_apply(%u, %u) PPL Start Pointer: %p Write Pointer: %p Block used: %d\n", 
	// 		block->page.id.space(), 
	// 		block->page.id.page_no(), 
	// 		block->page.static_ipl_pointer, 
	// 		block->page.ipl_write_pointer,
	// 		block->page.block_used);
}




// 세그먼트 경계를 넘어가는 로그 데이터를 처리하기 위한 보조 함수
byte* fetch_next_segment(byte* current_end, byte** new_end, byte** next_ppl) {
	if(next_ppl == NULL){
		fprintf(stderr, "Error : fetch_next_segment\n");
		return NULL;
	}
	*new_end = *next_ppl + nvdimm_info->each_ppl_size;
	byte * current_ptr = *next_ppl + NTH_IPL_HEADER_SIZE;
	byte * temp_ptr = get_addr_from_ipl_index(nvdimm_info->static_start_pointer, mach_read_from_4(*next_ppl), nvdimm_info->each_ppl_size);
	*next_ppl = temp_ptr;
	return current_ptr;
}

void apply_log_record(mlog_id_t log_type, byte* log_data, uint length, trx_id_t trx_id, buf_block_t* block, mtr_t* temp_mtr) {
	if (nvdimm_recv_ipl_undo && ipl_active_trx_ids.find(trx_id) != ipl_active_trx_ids.end()) {
			// pass	
			ib::info() << "skip undo because this is created from trx which is active at the crash!";	
	} 
	else {
		// fprintf(stderr, "log apply! (%u, %u) Type : %d len: %lu, apply_ptr: %p\n",block->page.id.space(), block->page.id.page_no(), log_type, length, log_data);
		recv_parse_or_apply_log_rec_body(
									log_type, log_data
									, log_data + length, block->page.id.space()
									, block->page.id.page_no(), block, temp_mtr);
	}
}

void insert_page_ipl_info_in_hash_table(buf_page_t * bpage){
	page_id_t page_id = bpage->id;
	std::pair <page_id_t, unsigned char *> insert_data = std::make_pair(bpage->id, bpage->static_ipl_pointer);
	buf_pool_t * buf_pool = normal_buf_pool_get(page_id);
	rw_lock_x_lock(&buf_pool->lookup_table_lock);
	buf_pool->ipl_look_up_table->insert(insert_data);
	rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	set_flag(&(bpage->flags), IN_LOOK_UP);
}

/* TODO Sjmun : 한 번도 Discard되지 않은 페이지들은 사실 IPL을 사용할 필요 없이 Global redo로그로만 복구가능한데..  */
void set_normalize_flag(buf_page_t * bpage, uint normalize_cause){
	if(bpage->normalize_cause == 0){
		//1. Record 이동
		//2. Max PPL Size를 넘어서는 경우
		//3. PPL 영역이 부족한 경우
		//4. PPL 대상 페이지가 아닌 경우
		//5. Cleaning
		bpage->normalize_cause = normalize_cause;
	}
	set_flag(&(bpage->flags), NORMALIZE);
}

/* Unset_flag를 해주지 않아도 Static_ipl이 free 되면 초기화 됨*/
void normalize_ipl_page(buf_page_t * bpage, page_id_t page_id){ 
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		switch (bpage->normalize_cause)
		{
			case 0:
				break;
			case 1:
				fprintf(stderr, "Normalize_cause,%f,Record_movement\n",(double)(time(NULL) - my_start));
				break;
			case 2:
				fprintf(stderr, "Normalize_cause,%f,Max_PPL_Size\n",(double)(time(NULL) - my_start));
				break;
			case 3:
				fprintf(stderr, "Normalize_cause,%f,PPL_Area_Lack\n",(double)(time(NULL) - my_start));
				break;
			case 4:
				fprintf(stderr, "Normalize_cause,%f,Not_PPL_Target\n",(double)(time(NULL) - my_start));
				break;
			case 5:
				fprintf(stderr, "Normalize_cause,%f,Cleaning\n",(double)(time(NULL) - my_start));
				break;
			default:
				break;
		}
	}
	else{
		switch (bpage->normalize_cause)
		{
			case 0:
				break;
			case 1:
				fprintf(stderr, "Normalize_cause,%f,Direct_Record_movement\n",(double)(time(NULL) - my_start));
				break;
			case 2:
				fprintf(stderr, "Normalize_cause,%f,Direct_Max_PPL_Size\n",(double)(time(NULL) - my_start));
				break;
			case 3:
				fprintf(stderr, "Normalize_cause,%f,Direct_PPL_Area_Lack\n",(double)(time(NULL) - my_start));
				break;
			case 4:
				fprintf(stderr, "Normalize_cause,%f,Direct_Not_PPL_Target\n",(double)(time(NULL) - my_start));
				break;
			case 5:
				fprintf(stderr, "Normalize_cause,%f,Direct_Cleaning\n",(double)(time(NULL) - my_start));
				break;
			default:
				break;
		}
	}
	if(get_flag(&(bpage->flags), IN_LOOK_UP)){
		buf_pool_t * buf_pool = normal_buf_pool_get(page_id);
		rw_lock_x_lock(&buf_pool->lookup_table_lock);
		buf_pool->ipl_look_up_table->erase(page_id);
		rw_lock_x_unlock(&buf_pool->lookup_table_lock);
	}
	bpage->static_ipl_pointer = NULL;
	bpage->ipl_write_pointer = NULL;
	bpage->block_used = 0;
	bpage->trx_id = 0;
	bpage->normalize_cause = 0;
	if(get_flag(&(bpage->flags), IN_PPL_BUF_POOL)){
		bpage->flags = 0;
		set_flag(&(bpage->flags), IN_PPL_BUF_POOL);
	}
	else{
		bpage->flags = 0;
	}
	
}



void set_for_ipl_page(buf_page_t* bpage){
	bpage->trx_id = 0;
	bpage->static_ipl_pointer = NULL;
	bpage->ipl_write_pointer = NULL;
	bpage->block_used = 0;
	bpage->flags = 0;
	bpage->normalize_cause = 0;
	page_id_t page_id = bpage->id;
	buf_pool_t * buf_pool = normal_buf_pool_get(page_id);
	rw_lock_s_lock(&buf_pool->lookup_table_lock);
	std::tr1::unordered_map<page_id_t, unsigned char * >::iterator it = buf_pool->ipl_look_up_table->find(page_id);
	rw_lock_s_unlock(&buf_pool->lookup_table_lock);
	if(it != buf_pool->ipl_look_up_table->end()){
		set_flag(&(bpage->flags), PPLIZED);
		set_flag(&(bpage->flags), IN_LOOK_UP);
		bpage->static_ipl_pointer = it->second;
	}
	
}


//Dynamic 영역을 가지고 있는 checkpoint page인지 확인하기.
// bool check_can_be_pplized(buf_page_t *bpage) {
//     // 플래그 검사 및 빠른 반환 조건
//     if (get_flag(&(bpage->flags), NORMALIZE)) {
//         return false;
//     }

//     if (get_flag(&(bpage->flags), DIRECTLY_WRITE)) {
//         return true;
//     }

//     // in_memory_ppl_size 계산
//     ulint in_memory_ppl_size = ((buf_block_t *)bpage)->in_memory_ppl_buf.size();

//     if (in_memory_ppl_size == 0) {
//         return false;
//     }

//     // PPLIZED 플래그에 따른 처리
//     if (!get_flag(&(bpage->flags), PPLIZED)) {
//         return alloc_first_ppl_to_bpage(bpage);
//     }

//     // 목표 길이 계산 및 크기 검사
//     ulint target_len = in_memory_ppl_size + get_ipl_length_from_ipl_header(bpage);
//     return target_len <= nvdimm_info->max_ppl_size;
// }


bool check_return_ppl_region(buf_page_t * bpage){
	if(!get_flag(&(bpage->flags), PPLIZED)){
		// fprintf(stderr, "Non PPLed Page (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
		bpage->static_ipl_pointer = NULL;
		bpage->ipl_write_pointer = NULL;
		bpage->trx_id = 0;
		bpage->block_used = 0;
		bpage->normalize_cause = 0;
		if(get_flag(&(bpage->flags), IN_PPL_BUF_POOL)){
			bpage->flags = 0;
			set_flag(&(bpage->flags), IN_PPL_BUF_POOL);
		}
		else{
			bpage->flags = 0;
		}
	}
	else{
		if(get_flag(&(bpage->flags), NORMALIZE)){
			// fprintf(stderr, "Normalize PPL Page (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
			normalize_ipl_page(bpage, bpage->id);
			return true;
		}
	}
	// fprintf(stderr, "PPLed Page (%u, %u)\n", bpage->id.space(), bpage->id.page_no());
	return false;
}

unsigned char * get_last_block_address_index(buf_page_t * bpage){
	if(mach_read_from_4(bpage->static_ipl_pointer + IPL_HDR_DYNAMIC_INDEX) == 0){
		return bpage->static_ipl_pointer + IPL_HDR_DYNAMIC_INDEX;
	}
	return (bpage->ipl_write_pointer - bpage->block_used);
}

void set_ipl_length_in_ipl_header(buf_page_t * bpage, ulint length){
	unsigned char * static_ipl_pointer = bpage->static_ipl_pointer;
	// fprintf(stderr, "Set IPL Length (%u, %u) Pointer : %p, Length : %lu\n", bpage->id.space(), bpage->id.page_no(), static_ipl_pointer + IPL_HDR_LEN, length);
	mach_write_to_4(static_ipl_pointer + IPL_HDR_LEN, length);
	flush_cache(static_ipl_pointer + IPL_HDR_LEN, 4);
}

uint get_ipl_length_from_ipl_header(buf_page_t * bpage){
	// fprintf(stderr, "Get IPL Length (%u, %u) Pointer : %p, length: %u\n", bpage->id.space(), bpage->id.page_no(), bpage->static_ipl_pointer + IPL_HDR_LEN, mach_read_from_4(bpage->static_ipl_pointer + IPL_HDR_LEN));
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

// #define NT_THRESHOLD (2 * CACHE_LINE_SIZE)

// void memcpy_to_cxl(void *__restrict dst, const void * __restrict src, size_t n)
// {
// 	if (n < NT_THRESHOLD) {
// 		memcpy(dst, src, n);
// 		return;
// 	}

// 	size_t n_unaligned = CACHE_LINE_SIZE - (uintptr_t)dst % CACHE_LINE_SIZE;

// 	if (n_unaligned > n)
// 		n_unaligned = n;

// 	memcpy(dst, src, n_unaligned);
// 	dst += n_unaligned;
// 	src += n_unaligned;
// 	n -= n_unaligned;

// 	size_t num_lines = n / CACHE_LINE_SIZE;

// 	size_t i;
// 	for (i = 0; i < num_lines; i++) {
// 		size_t j;
// 		for (j = 0; j < CACHE_LINE_SIZE / sizeof(__m128i); j++) {
// 			__m128i blk = _mm_loadu_si128((const __m128i *)src);
// 			/* non-temporal store */
// 			_mm_stream_si128((__m128i *)dst, blk);
// 			src += sizeof(__m128i);
// 			dst += sizeof(__m128i);
// 		}
// 		n -= CACHE_LINE_SIZE;
// 	}

// 	if (num_lines > 0)
// 		_mm_sfence();

// 	memcpy(dst, src, n);
// }


void memset_to_cxl(void* dest, int value, size_t size){
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
#endif