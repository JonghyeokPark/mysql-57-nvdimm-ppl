
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
#include <vector>
#include "page0page.h"
#include "buf0flu.h"
#include "srv0srv.h"
#include "srv0mon.h"
#include "row0row.h"
#include "trx0sys.h"
#include "row0vers.h"
#include "trx0undo.h"

bool ppl_is_llt_view(const ReadView* view) {
	if (view == NULL) return false;
	return view->m_is_llt;
}

/* Clock-sweep cache for prebuilt page snapshots.
   16MB / 4KB page = 4096 slots. Each slot has a reference bit; on hit
   set ref_bit=1; on insert without free slot, advance clock hand,
   clearing ref_bit=1 entries to 0 and evicting the first ref_bit=0. */
namespace {
struct prebuilt_slot_t {
	uint64_t     key;
	buf_page_t*  page;
	uint8_t      ref_bit;
	uint32_t     refcount;   /* # of in-flight readers using this slot */
	bool         used;
	prebuilt_slot_t() : key(0), page(NULL), ref_bit(0), refcount(0), used(false) {}
};
const size_t PREBUILT_CACHE_CAP = (128ULL * 1024 * 1024) / 4096;   /* = 8192 */
std::vector<prebuilt_slot_t> g_prebuilt_slots(PREBUILT_CACHE_CAP);
std::tr1::unordered_map<uint64_t, size_t> g_prebuilt_slot_idx;
size_t g_prebuilt_clock_hand = 0;
}

void ppl_clear_prebuilt_cache(void) {
	mutex_enter(&prebuilt_page_list_mutex);
	for (size_t i = 0; i < PREBUILT_CACHE_CAP; i++) {
		if (g_prebuilt_slots[i].used && g_prebuilt_slots[i].page != NULL) {
			ut_free(buf_page_get_block(g_prebuilt_slots[i].page));
		}
		g_prebuilt_slots[i] = prebuilt_slot_t();
	}
	g_prebuilt_slot_idx.clear();
	g_prebuilt_clock_hand = 0;
	prebuilt_page_list.clear();
	mutex_exit(&prebuilt_page_list_mutex);
}

/* Snapshot leaf data pages of given tablespace into prebuilt cache.
   Skip system pages (0=FSP, 1=IBUF, 2=INODE). Pages not in BP are
   force-loaded via buf_page_get_gen. */
void ppl_snapshot_space_for_llt(ulint space_id, trx_id_t llt_ts) {
	if (space_id == 0 || llt_ts == 0) return;
	ulint pages = fil_space_get_size(space_id);
	if (pages == 0) return;
	if (pages > 256) pages = 256;  /* safety cap */
	bool found;
	const page_size_t page_size(fil_space_get_page_size(space_id, &found));
	if (!found) return;
	ulint added = 0;
	for (ulint p = 3; p < pages; p++) {  /* skip 0,1,2 system pages */
		page_id_t pid(space_id, p);
		mtr_t mtr;
		mtr_start(&mtr);
		buf_block_t* block = buf_page_get_gen(pid, page_size, RW_S_LATCH,
			NULL, BUF_GET, __FILE__, __LINE__, &mtr, false);
		if (block != NULL) {
			ulint ptype = mach_read_from_2(block->frame + FIL_PAGE_TYPE);
			if (ptype == FIL_PAGE_INDEX || ptype == FIL_PAGE_RTREE) {
				add_prebuilt_page(&block->page, llt_ts);
				added++;
			}
		}
		mtr_commit(&mtr);
	}
	(void)added;  /* silenced: snapshot summary log removed */
}

#include <emmintrin.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

prebuilt_page_map_t prebuilt_page_list;
/* Guards prebuilt_page_list against concurrent insert/find/erase
   from OLTP de-PPLization adders and LLT version-build consumers. */
ib_mutex_t prebuilt_page_list_mutex;

static inline uint64_t prebuilt_key(ulint space, ulint page_no) {
    return ((uint64_t)space << 32) | (uint32_t)page_no;
}

/* Per-thread out-parameter set by find_prebuilt_page_from_list; consumed
   by PrebuiltGuard right after find. (size_t)-1 = no slot held. */
static __thread size_t tls_last_prebuilt_slot_idx = (size_t)-1;

/* RAII guard: auto-release on scope exit. Lock-free atomic_dec.
   Evict requires refcount==0, so the slot stays valid while held. */
namespace { struct PrebuiltGuard {
    size_t slot_idx;
    PrebuiltGuard() : slot_idx((size_t)-1) {}
    ~PrebuiltGuard() {
        if (slot_idx != (size_t)-1) {
            os_atomic_decrement_uint32(
                &g_prebuilt_slots[slot_idx].refcount, 1);
        }
    }
}; }

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

	unsigned char* ppl = apply_page->first_ppl_block_ptr;
	if (ppl == NULL) return;

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

	/* Post-apply page sanity check. Catch stale-chain corruption that
	   silently wrote out-of-range bytes into the page (heap_top, n_recs,
	   page type). If we crash here, the chain that was just applied is
	   the suspect — apply log_size and ppl pointer recorded above for
	   the trace. */
	// if (!nvdimm_recv_running) {
	// 	page_t* page = block->frame;
	// 	ulint heap_top = mach_read_from_2(page + PAGE_HEADER + PAGE_HEAP_TOP);
	// 	ulint n_recs   = mach_read_from_2(page + PAGE_HEADER + PAGE_N_RECS);
	// 	ulint ptype    = mach_read_from_2(page + FIL_PAGE_TYPE);
	// 	bool corrupt = false;
	// 	if (heap_top > UNIV_PAGE_SIZE || heap_top < PAGE_DATA) corrupt = true;
	// 	if (n_recs   > 8000)                                   corrupt = true;
	// 	if (ptype != FIL_PAGE_INDEX
	// 	 && ptype != FIL_PAGE_RTREE)                           corrupt = true;
	// 	if (corrupt) {
	// 		fprintf(stderr,
	// 			"PPL_APPLY_CORRUPT,page=%u:%u,heap_top=%lu,n_recs=%lu,ptype=%lu\n",
	// 			(unsigned)block->page.id.space(),
	// 			(unsigned)block->page.id.page_no(),
	// 			heap_top, n_recs, ptype);
	// 		fflush(stderr);
	// 		abort();
	// 	}
	// }
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
				// fprintf(stderr, "Normalize_cause,%f,PPL_Area_Lack\n",(double)(time(NULL) - my_start));
				break;
			case 4:
				// fprintf(stderr, "Normalize_cause,%f,Not_PPL_Target\n",(double)(time(NULL) - my_start));
				break;
			case 5:
				// fprintf(stderr, "Normalize_cause,%f,Cleaning\n",(double)(time(NULL) - my_start));
				break;
			case 6:
				// fprintf(stderr, "Normalize_cause,%f,LSN_GAP_Too_Large\n",(double)(time(NULL) - my_start));
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
				// fprintf(stderr, "Normalize_cause,%f,Direct_PPL_Area_Lack\n",(double)(time(NULL) - my_start));
				break;
			case 4:
				// fprintf(stderr, "Normalize_cause,%f,Direct_Not_PPL_Target\n",(double)(time(NULL) - my_start));
				break;
			case 5:
				// fprintf(stderr, "Normalize_cause,%f,Direct_Cleaning\n",(double)(time(NULL) - my_start));
				break;
			case 6:
				// fprintf(stderr, "Normalize_cause,%f,Direct_LSN_GAP_Too_Large\n",(double)(time(NULL) - my_start));
				break;
			default:
				break;
		}
	}
}

void normalize_ppled_page(buf_page_t * bpage, page_id_t page_id){
	/* Capture cause and PPL byte count BEFORE clearing the bpage fields
	   below, for innodb_metrics. */
	{
		uint cause = bpage->normalize_cause;
		ulint log_bytes = bpage->ppl_length;
		MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_TOTAL);
		MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LOG_BYTES_TOTAL, log_bytes);
		switch (cause) {
		case 1: MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_CAUSE1); break;
		case 2: MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_CAUSE2); break;
		case 3: MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_CAUSE3); break;
		case 4: MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_CAUSE4); break;
		case 5: MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_CAUSE5); break;
		case 6: MONITOR_INC(MONITOR_NVDIMM_PPL_NORMALIZE_CAUSE6); break;
		default: break;
		}
	}
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
			/* PPL-MVCC prebuild snapshot is taken earlier in
			   buf_page_io_complete WHILE the SX-lock is still held;
			   here we only truncate the chain. */
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
	/* Guard against PPLizing non-index pages (FSP_HDR/XDES, IBUF_BITMAP,
	   INODE, UNDO, etc). page_is_leaf only checks PAGE_LEVEL bytes which
	   on non-index pages are arbitrary data and may happen to be zero,
	   causing false-positive PPLization → record-level apply on a
	   non-index page byte layout → page corruption. */
	ulint ptype = mach_read_from_2(((buf_block_t*)buf_page)->frame + FIL_PAGE_TYPE);
	if(!is_system_or_undo_tablespace(space) &&
		!get_flag(&(buf_page->flags), NORMALIZE) &&
		(ptype == FIL_PAGE_INDEX || ptype == FIL_PAGE_RTREE) &&
		page_is_leaf(((buf_block_t *)buf_page)->frame) &&
		buf_page_in_file(buf_page) &&
		page_id.page_no() > 7){
		return true;
	}
	set_normalize_flag(buf_page, 4);
	return false;
}


void
init_prebuilt_page_cache(prebuilt_page_map_t& prebuilt_page_list){
    prebuilt_page_list.clear();
}

buf_page_t*
add_prebuilt_page(buf_page_t* bpage, trx_id_t target_ts){
    /* Snapshot a buf_block_t control struct + the 16KB frame into a
       single ut_malloc allocation so we can ut_free it cleanly later.
       Layout: [buf_block_t][padding][frame aligned to UNIV_PAGE_SIZE].
       Locks/mutexes inside the copied buf_block_t are bitwise dupes
       and must NOT be acquired on the copy. */
    buf_block_t* current_block = buf_page_get_block(bpage);
    if (current_block == NULL || current_block->frame == NULL) {
        return NULL;
    }

    const size_t total_size = sizeof(buf_block_t)
                            + UNIV_PAGE_SIZE - 1  /* alignment slack */
                            + UNIV_PAGE_SIZE;     /* frame */
    byte* raw = static_cast<byte*>(ut_malloc_nokey(total_size));
    if (raw == NULL) return NULL;

    buf_block_t* prebuilt_block = reinterpret_cast<buf_block_t*>(raw);
    byte* frame = static_cast<byte*>(ut_align(
        raw + sizeof(buf_block_t), UNIV_PAGE_SIZE));

    memcpy(prebuilt_block, current_block, sizeof(buf_block_t));
    buf_frame_copy(frame, current_block->frame);
    prebuilt_block->frame = frame;
    prebuilt_block->page.trx_id = target_ts;

    buf_page_t* prebuilt_page = &prebuilt_block->page;
    uint64_t key = prebuilt_key(bpage->id.space(), bpage->id.page_no());

    buf_page_t* old = NULL;
    buf_page_t* kept = NULL;
    buf_page_t* evicted = NULL;
    bool        discarded = false;

    mutex_enter(&prebuilt_page_list_mutex);
    std::tr1::unordered_map<uint64_t, size_t>::iterator idx_it
        = g_prebuilt_slot_idx.find(key);
    if (idx_it != g_prebuilt_slot_idx.end()) {
        /* Same page: reuse slot, keep older trx_id (closer to LLT view). */
        size_t slot = idx_it->second;
        buf_page_t* existing = g_prebuilt_slots[slot].page;
        if (existing->trx_id <= target_ts) {
            kept = existing;
        } else {
            old = existing;
            g_prebuilt_slots[slot].page = prebuilt_page;
        }
        g_prebuilt_slots[slot].ref_bit = 1;
    } else {
        /* New page: find empty slot or clock-evict (refcount==0 only). */
        size_t slot = (size_t)-1;
        for (size_t i = 0; i < PREBUILT_CACHE_CAP; i++) {
            if (!g_prebuilt_slots[i].used) { slot = i; break; }
        }
        if (slot == (size_t)-1) {
            /* Clock sweep: skip in-flight (refcount>0), clear ref_bit=1,
               evict first ref_bit==0 && refcount==0. */
            for (size_t tries = 0; tries < 2 * PREBUILT_CACHE_CAP; tries++) {
                size_t i = (g_prebuilt_clock_hand + tries) % PREBUILT_CACHE_CAP;
                if (g_prebuilt_slots[i].refcount > 0) continue;
                if (g_prebuilt_slots[i].ref_bit == 0) {
                    /* Evict: defer ut_free until mutex released. */
                    evicted = g_prebuilt_slots[i].page;
                    g_prebuilt_slot_idx.erase(g_prebuilt_slots[i].key);
                    slot = i;
                    g_prebuilt_clock_hand = (i + 1) % PREBUILT_CACHE_CAP;
                    break;
                }
                g_prebuilt_slots[i].ref_bit = 0;
            }
        }
        if (slot == (size_t)-1) {
            /* Everyone is pinned (refcount>0) — give up, discard. */
            discarded = true;
        } else {
            g_prebuilt_slots[slot].key = key;
            g_prebuilt_slots[slot].page = prebuilt_page;
            g_prebuilt_slots[slot].ref_bit = 1;
            g_prebuilt_slots[slot].refcount = 0;
            g_prebuilt_slots[slot].used = true;
            g_prebuilt_slot_idx[key] = slot;
        }
    }
    mutex_exit(&prebuilt_page_list_mutex);

    if (kept != NULL) {
        ut_free(prebuilt_block);
        return kept;
    }
    if (discarded) {
        ut_free(prebuilt_block);
        return NULL;
    }
    if (old != NULL) {
        ut_free(buf_page_get_block(old));
    }
    if (evicted != NULL) {
        ut_free(buf_page_get_block(evicted));
    }
    return prebuilt_page;
}


void
remove_prebuilt_page_from_list(buf_page_t* prebuilt_page, prebuilt_page_map_t& prebuilt_page_list){
    if (prebuilt_page == NULL) return;
    uint64_t key = prebuilt_key(prebuilt_page->id.space(),
                                prebuilt_page->id.page_no());
    (void) prebuilt_page_list;
    buf_page_t* to_free = NULL;
    mutex_enter(&prebuilt_page_list_mutex);
    std::tr1::unordered_map<uint64_t, size_t>::iterator idx_it
        = g_prebuilt_slot_idx.find(key);
    if (idx_it != g_prebuilt_slot_idx.end()
        && g_prebuilt_slots[idx_it->second].page == prebuilt_page) {
        to_free = g_prebuilt_slots[idx_it->second].page;
        g_prebuilt_slots[idx_it->second] = prebuilt_slot_t();
        g_prebuilt_slot_idx.erase(idx_it);
    }
    mutex_exit(&prebuilt_page_list_mutex);
    if (to_free != NULL) {
        ut_free(buf_page_get_block(to_free));
    }
}

buf_page_t*
find_prebuilt_page_from_list(
	buf_page_t*			target_bpage,
	prebuilt_page_map_t&		prebuilt_page_list,
	ReadView*			reader_view,
	const table_name_t&		table_name)
{
    /* Hash lookup keyed by (space, page_no). One entry per page (add
       replaces old). Reader does per-record visibility check after
       extraction, so we don't filter on tag here. */
    (void) reader_view;
    (void) table_name;
    (void) prebuilt_page_list;
    if (target_bpage == NULL) return NULL;
    uint64_t key = prebuilt_key(target_bpage->id.space(),
                                target_bpage->id.page_no());
    buf_page_t* found = NULL;
    mutex_enter(&prebuilt_page_list_mutex);
    std::tr1::unordered_map<uint64_t, size_t>::iterator idx_it
        = g_prebuilt_slot_idx.find(key);
    tls_last_prebuilt_slot_idx = (size_t)-1;
    if (idx_it != g_prebuilt_slot_idx.end()) {
        found = g_prebuilt_slots[idx_it->second].page;
        g_prebuilt_slots[idx_it->second].ref_bit = 1;
        g_prebuilt_slots[idx_it->second].refcount++;
        tls_last_prebuilt_slot_idx = idx_it->second;
    }
    mutex_exit(&prebuilt_page_list_mutex);
    bool is_llt = ppl_is_llt_view(reader_view);
    if (found == NULL) {
        MONITOR_INC(MONITOR_NVDIMM_PPL_PREBUILD_MISS);
        if (is_llt) MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PREBUILD_MISS);
    } else {
        MONITOR_INC(MONITOR_NVDIMM_PPL_PREBUILD_HIT);
        if (is_llt) MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PREBUILD_HIT);
    }
    return found;
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

	/* Per-path tag for accounting:
	     0 = none/early-error
	     1 = Path A (prebuilt direct success — visible)
	     2 = Path B (prebuilt found, visibility failed → DB_FAIL → undo)
	     3 = Path C (disk + PPL apply) */
	int __path_tag = 0;
	bool __is_llt = ppl_is_llt_view(read_view);
	uint64_t __find_us = 0;  /* time spent in find_prebuilt */
	int __chain_len = 0;     /* path B nested-undo or path C apply-count */
	/* RAII timer + outcome accounting. */
	struct RedoBuildStats {
		rec_t** old_vers_p;
		int* path_p;
		uint64_t* find_p;
		int* chain_p;
		bool is_llt;
		ib_uint64_t start;
		RedoBuildStats(rec_t** ov, int* pp, uint64_t* fp, int* cp, bool llt)
			: old_vers_p(ov), path_p(pp), find_p(fp), chain_p(cp),
			  is_llt(llt), start(ut_time_us(NULL)) {}
		~RedoBuildStats() {
			uint64_t total = (uint64_t)(ut_time_us(NULL) - start);
			uint64_t fus = find_p ? *find_p : 0;
			/* pure version-construction time = total - find */
			uint64_t us = (total > fus) ? (total - fus) : 0;
			MONITOR_INC(MONITOR_NVDIMM_PPL_REDO_BUILD_CALLS);
			MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_REDO_BUILD_US_SUM, (mon_type_t)us);
			MONITOR_SET_UPD_MAX_ONLY(MONITOR_NVDIMM_PPL_REDO_BUILD_US_MAX, (mon_type_t)us);
			if (is_llt) {
				MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_REDO_BUILD_CALLS);
				MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_REDO_BUILD_US_SUM, (mon_type_t)us);
			}
			if (old_vers_p && *old_vers_p != NULL) {
				MONITOR_INC(MONITOR_NVDIMM_PPL_REDO_OUT_VERS);
			} else {
				MONITOR_INC(MONITOR_NVDIMM_PPL_REDO_OUT_NULL);
			}
			switch (*path_p) {
			case 1: /* Path A */
				MONITOR_INC(MONITOR_NVDIMM_PPL_PATH_A_CALLS);
				MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_PATH_A_US_SUM, (mon_type_t)us);
				MONITOR_SET_UPD_MAX_ONLY(MONITOR_NVDIMM_PPL_PATH_A_US_MAX, (mon_type_t)us);
				if (is_llt) {
					MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PATH_A_CALLS);
					MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_A_US_SUM, (mon_type_t)us);
				}
				break;
			case 2: /* Path B fallback */
				MONITOR_INC(MONITOR_NVDIMM_PPL_PATH_B_FALLBACK);
				if (is_llt) {
					MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PATH_B_FALLBACK);
					MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_B_US_SUM, (mon_type_t)us);
					if (chain_p) {
						MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_B_LEN_SUM,
							(mon_type_t)*chain_p);
					}
				}
				break;
			case 3: /* Path C pure: disk + forward apply, result visible */
				if (old_vers_p && *old_vers_p != NULL) {
					MONITOR_INC(MONITOR_NVDIMM_PPL_PATH_C_CALLS);
					MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_PATH_C_US_SUM, (mon_type_t)us);
					MONITOR_SET_UPD_MAX_ONLY(MONITOR_NVDIMM_PPL_PATH_C_US_MAX, (mon_type_t)us);
					if (is_llt) {
						MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PATH_C_CALLS);
						MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_C_US_SUM, (mon_type_t)us);
						if (chain_p) {
							MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_C_LEN_SUM,
								(mon_type_t)*chain_p);
						}
					}
				} else {
					MONITOR_INC(MONITOR_NVDIMM_PPL_PATH_C_FAIL);
				}
				break;
			case 4: /* Path C + nested undo (Old Page + Undo) */
				if (is_llt && old_vers_p && *old_vers_p != NULL) {
					MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PATH_C_NESTED_UNDO_CALLS);
					MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_C_NESTED_UNDO_US_SUM,
						(mon_type_t)us);
				}
				break;
			case 5: /* Disk too new → undo from disk frame */
				if (is_llt && old_vers_p && *old_vers_p != NULL) {
					MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PATH_C_DISK_UNDO_CALLS);
					MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_C_DISK_UNDO_US_SUM,
						(mon_type_t)us);
					if (chain_p) {
						MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH_C_DISK_UNDO_LEN_SUM,
							(mon_type_t)*chain_p);
					}
				}
				break;
			default: break;
			}
			/* Find time as separate metric. */
			if (find_p && is_llt && *find_p > 0) {
				MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_FIND_CALLS);
				MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_FIND_US_SUM,
					(mon_type_t)*find_p);
			}
		}
	} __redo_timer(old_vers, &__path_tag, &__find_us, &__chain_len, __is_llt);
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
	bool used_prebuilt = false;
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

	/* PPL chain meta. For non-PPLized pages (e.g. warehouse) the
	   chain pointers are NULL; we still proceed because the prebuilt
	   cache may have a snapshot for this page. The disk-read /
	   chain-apply path won't run if apply_log_size stays 0. */
	apply_log_size = 0;
	start_ptr = bpage->first_ppl_block_ptr;
	byte *current_ptr = NULL;
	byte *next_ppl = NULL;
	if (start_ptr != NULL) {
		apply_log_size = get_ppl_length_from_ppl_header(bpage);
		current_ptr = start_ptr + PPL_BLOCK_HDR_SIZE;
		end_ptr = start_ptr + nvdimm_info->each_ppl_size;
		next_ppl = get_addr_from_ppl_index(nvdimm_info->ppl_start_pointer,
			mach_read_from_4(start_ptr + PPL_HDR_DYNAMIC_INDEX),
			nvdimm_info->each_ppl_size);
	}
    byte temp_buffer[400] ={0, }; // Temporary buffer to handle data that spans multiple segments
	mlog_id_t log_type;
	ulint log_body_length;
	trx_id_t trx_id;


	/* temp_page allocation deferred to Path C entry. Prebuilt path
	   points temp_page directly at pre_block->frame (no alloc). */

	if(nvdimm_info->old_page==NULL){
		nvdimm_info->old_page = (byte *)calloc(UNIV_PAGE_SIZE, sizeof(char));
		buf_frame_copy(nvdimm_info->old_page, page);
	}


	/* Removed racy global last-built-page cache (nvdimm_info->old_page
	   reuse). Path B prebuilt cache handles cross-call reuse with
	   proper readview matching; the global cache was thread-unsafe and
	   could TOCTOU between the page_no check here and buf_frame_copy
	   below, producing a temp_page from a different page → cyclic record
	   list → hang in page_find_rec_with_heap_no. */

	/* Prebuilt page lookup (paper §5.2). Time the lookup separately
	   so it can be excluded from path_*_us_sum (pure version-build
	   cost) and tracked as its own metric. */
	PrebuiltGuard __pg;
	{
		ib_uint64_t __find_start = ut_time_us(NULL);
		temp_bpage = find_prebuilt_page_from_list(bpage, prebuilt_page_list,
	                                           read_view,
	                                           clust_index->table->name);
		__find_us = (uint64_t)(ut_time_us(NULL) - __find_start);
		__pg.slot_idx = tls_last_prebuilt_slot_idx;   /* lock-free release on exit */
	}
	if (temp_bpage != NULL) {
		buf_block_t* pre_block = buf_page_get_block(temp_bpage);
		if (pre_block != NULL && pre_block->frame != NULL) {
			/* Point temp_page directly at prebuilt frame instead of
			   copying 16KB. Prebuilt cache stays alive for LLT lifetime,
			   so the frame is stable for the duration of this function. */
			temp_page = pre_block->frame;
			used_prebuilt = true;
		}
		/* Do NOT consume-on-read: paper §5.2 keeps prebuilt usable
		   for the entire LLT lifetime; multiple records on the same
		   page hit the same prebuilt repeatedly. Cleanup happens at
		   LLT commit (not yet implemented). */
		if (used_prebuilt) {
			__path_tag = 1; /* Path A; may downgrade to 2 if visibility check fails below */
			goto get_rec_offset;
		}
	}
	
	__path_tag = 3; /* Path C attempted (disk read + PPL apply) */

read_old_page:
	/* Read the old page from the disk */

	buf1 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	old_page = static_cast<byte*>(ut_align(buf1, UNIV_PAGE_SIZE));


	//fprintf(stderr, "Before locking space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);
	//mutex_enter(buf_page_get_mutex(bpage));
	//rw_lock_s_lock_gen(&((buf_block_t*) bpage)->lock,BUF_IO_READ);

	/* Read the disk version directly into our local old_page buffer.
	   Do NOT go through buf_page_init_for_read — that function tries to
	   register a NEW BP entry and returns NULL when the page is already
	   cached (which is always our case, since caller holds an S-latch on
	   the very page we want the disk version of). The previous call
	   silently DB_FAIL'd 100% of the time, making this entire path C
	   codepath dead. fil_io reads directly from the tablespace file
	   into our heap buffer, bypassing BP. */
	err = fil_io(IORequestRead, true, page_id, page_size, 0, page_size.physical(),
			old_page, NULL);

	//rw_lock_s_unlock_gen(&((buf_block_t*) bpage)->lock,BUF_IO_READ);
	//mutex_exit(buf_page_get_mutex(bpage));		

	if(err!=DB_SUCCESS){
		bpage->io_fix==BUF_IO_NONE;
		fprintf(stderr, "failed to read an old page from the disk. space_id: %d page_no: %lu\n", page_id.space(), page_id.page_no());
		*old_vers = NULL;
		return DB_FAIL;
	}

	//fprintf(stderr, "After fil_io space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);
	//fprintf(stderr, "After lock release space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);

	/* Path C: read max_trx from the freshly disk-loaded local old_page,
	   not the stale nvdimm_info->old_page global cache. */
	old_page_max_trx_id = page_get_max_trx_id(old_page);
	cur_page_max_trx_id = page_get_max_trx_id(page);

	/* If even the disk-loaded version is already newer than the view's
	   snapshot threshold, forward apply can't help (each PPL moves
	   max_trx forward). Instead of bailing out so the caller walks
	   undo from the latest BP record (longest chain), use the disk
	   frame's record as the undo starting point — its trx_id ≤ disk
	   max so the undo walk from there is shorter. We tag path_tag=5
	   and skip the apply loop; the existing nested undo block below
	   will fire because the disk record is invisible. */
	if (!read_view->changes_visible(old_page_max_trx_id,
	                                clust_index->table->name)) {
		__path_tag = 5;
	}
	

	mtr_start(&temp_mtr);
	mtr_set_log_mode(&temp_mtr, MTR_LOG_NONE);

	/* Allocate temp_page only when entering Path C (prebuilt path uses
	   pre_block->frame directly, no allocation needed). */
	buf2 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	temp_page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	/* Initialize temp_page to disk state so it's never uninitialized,
	   even if the apply loop below doesn't iterate (e.g. disk's max_trx
	   already not visible to LLT — extremely rare since LLT.start is
	   usually older than disk-flushed trx). */
	buf_frame_copy(temp_page, old_page);

	/* 3. Traverse PPL log records and apply only those visible to LLT
	   readview. PPL records are stored in commit-time order, so visibility
	   is a monotone function: once we find a record invisible to the view,
	   all subsequent records are also invisible. Break out without applying.
	   Then old_page holds the last LLT-visible state. */
	while (apply_log_size != 0) {
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

		/* Visibility check on THIS PPL record's trx_id. PPL is in commit
		   order so a single invisible record means all subsequent ones
		   are too — break without applying. old_page state preserved. */
		if (!read_view->changes_visible(trx_id, clust_index->table->name)) {
			break;
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
					*old_vers = NULL;
					temp_mtr.discard_modifications();
					mtr_commit(&temp_mtr);
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
		/* Re-read max_trx from local old_page after (possible) apply. */
		old_page_max_trx_id = page_get_max_trx_id(old_page);
    }

	/* After loop, old_page holds all LLT-visible applied state.
	   Copy once into temp_page (the rec extraction target). */
	buf_frame_copy(temp_page, old_page);
	temp_trx_id = old_page_max_trx_id;

	/* Path C chain length = number of PPL log records forward-applied. */
	__chain_len = IPL_apply_cnt;

	/* Close the apply mtr now that forward apply is done. The
	   nvdimm_recv_parse_or_apply_log_rec_body calls inside the loop
	   above may have S-latched ancillary pages via temp_mtr; leaving
	   the mtr open holds those latches until the thread's next
	   mtr_commit, causing self-deadlock when the same thread later
	   tries to X-latch a page it transitively S-latched here. */
	temp_mtr.discard_modifications();
	mtr_commit(&temp_mtr);

	//fprintf(stderr, "finished apply: space_id: %d page_no: %lu IPL_apply_cnt: %d bpage: %lu\n", page_id.space(), page_id.page_no(), IPL_apply_cnt, bpage);

	/* 4. After getting the right version of the IPL page, store the right record to the old_vers record */

get_rec_offset:
	if (used_prebuilt) {
		/* temp_page already populated from prebuilt frame above.
		   Don't overwrite it with nvdimm_info->old_page. */
	} else if (resuse_prev_built_page == true) {
		buf_frame_copy(temp_page, nvdimm_info->old_page);
	}

	*offsets = rec_get_offsets(
			rec, clust_index, *offsets, ULINT_UNDEFINED,
			offset_heap);

	//page_rec_print(rec, *offsets);


	if(*offsets==NULL){
		fprintf(stderr, "offset NULL during redo version creation\n");
		*old_vers = NULL;
		return DB_FAIL;
	}

	// cannot access old_vers -> need to

	const rec_t* temp_page_rec;
	ulint rec_slot_index = page_dir_find_owner_slot(rec);
	ulint heap_no = page_rec_get_heap_no(rec);

	temp_page_rec = page_find_rec_with_heap_no(temp_page, heap_no);

	if (temp_page_rec == NULL) {
		*old_vers = NULL;
		return DB_FAIL;
	}

	ulint*		temp_offsets = NULL;
	mem_heap_t*	temp_offset_heap		= NULL;

	temp_offset_heap = mem_heap_create(1024);

	temp_offsets = rec_get_offsets(
			temp_page_rec, clust_index, temp_offsets, ULINT_UNDEFINED,
			&temp_offset_heap);

	byte* buf = static_cast<byte*>(
				mem_heap_alloc(
					in_heap, rec_offs_size(temp_offsets)));


	*old_vers = rec_copy(buf, temp_page_rec, *offsets); // copy old vers to buf
	rec_offs_make_valid(*old_vers, clust_index, *offsets);

	if(rec_get_deleted_flag(*old_vers, true)){
		*old_vers = NULL;
		return DB_FAIL;
	}

	/* paper §5.2 [2]: extracted record from prebuilt (or disk-applied)
	   frame is too new for the LLT view. Walk undo backward starting
	   from this version (instead of from the buffer-pool current
	   version) — shorter walk than from the BP top.

	   Note: temp_page_rec is in our local 16KB buffer (not in BP).
	   row_vers_build_for_consistent_read has a debug assertion that
	   the rec's page is in mtr's latch list; that assertion compiles
	   out in release builds. Functionally the function only reads
	   roll_ptr from rec and walks undo log pages (which ARE in BP). */
	{
		trx_id_t old_rec_trx_id = row_get_rec_trx_id(*old_vers, clust_index, *offsets);
		if (!read_view->changes_visible(old_rec_trx_id, clust_index->table->name)) {
			/* #4 Old Page+Undo path removed: if Path C apply finished but
			   result still invisible, return DB_FAIL so caller falls back
			   to vanilla undo (#5 Latest+Undo from BP) instead of nested
			   undo from disk page. Same for #5 (disk-too-new). */
			if (__path_tag == 3 || __path_tag == 5) {
				*old_vers = NULL;
				return DB_FAIL;
			}
			/* Downgrade path tag based on prior state:
			     1 (path A) → 2 (path B: prebuilt + nested undo) */
			if (__path_tag == 1) {
				__path_tag = 2;
			}
			rec_t* undo_old_vers = NULL;
			ib_uint64_t __p2_start = ut_time_us(NULL);
			tls_llt_undo_chain_len = 0;
			dberr_t undo_err = row_vers_build_for_consistent_read(
				temp_page_rec, mtr, clust_index, offsets,
				read_view, offset_heap, in_heap,
				&undo_old_vers, vrow);
			uint64_t p2_us = (uint64_t)(ut_time_us(NULL) - __p2_start);
			__chain_len = tls_llt_undo_chain_len;
			/* Track p2 portion (pure undo walk time) only for path B,
			   to preserve historical path_b vs path2_undo decomposition.
			   Path 4 and 5 get their total time from destructor cases. */
			if (__is_llt && __path_tag == 2) {
				MONITOR_INC(MONITOR_NVDIMM_PPL_LLT_PATH2_UNDO_CALLS);
				MONITOR_INC_VALUE(MONITOR_NVDIMM_PPL_LLT_PATH2_UNDO_US_SUM,
					(mon_type_t)p2_us);
			}
			/* Long-call trace: dump page_id + heap_no when one paper [2]
			   undo walk takes > 100 ms — candidate for the long-latch
			   semaphore wait. */
			if (p2_us > 100000) {
				ulint hn = (temp_page_rec != NULL) ?
					page_rec_get_heap_no(temp_page_rec) : 0;
				fprintf(stderr,
					"PAPER2_LONG,page=%u:%u,heap_no=%lu,us=%lu,err=%d\n",
					(unsigned)bpage->id.space(),
					(unsigned)bpage->id.page_no(),
					hn, (unsigned long)p2_us, (int)undo_err);
				fflush(stderr);
			}
			if (undo_err == DB_SUCCESS && undo_old_vers != NULL) {
				*old_vers = undo_old_vers;
				return DB_SUCCESS;
			}
			*old_vers = NULL;
			return DB_FAIL;
		}
	}

	/* Removed write-back to racy global cache. */

	if (vrow && *vrow) {
		*vrow = dtuple_copy(*vrow, in_heap);
		dtuple_dup_v_fld(*vrow, in_heap);
	}

	//fprintf(stderr, "End of func space_id: %d page_no: %lu lock: %lu\n", page_id.space(), page_id.page_no(), block->lock);



	return DB_SUCCESS;
}
#endif
