#ifdef UNIV_NVDIMM_IPL
#include "nvdimm-ipl.h"
#include "mtr0log.h"
#include "page0page.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include <emmintrin.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

/** Hashed page file address struct */
struct ppl_page_t{
	enum recv_addr_state state;
				/*!< recovery state of the page */
	unsigned	space:32;/*!< space id */
	unsigned	page_no:32;/*!< page number */
};

ulint
ppl_buf_page_read_in_area(
	std::vector <std::pair<page_id_t, uint64_t> >	page_id_list /*Page_id List*/,
	uint read_page_no,
	buf_pool_t * buf_pool)
{
	ulint	n;

	n = 0;

	for (uint i = 0; i < page_id_list.capacity() && n < read_page_no; i++) {
		const page_id_t cur_page_id = page_id_list[i].first;
		buf_pool_t * page_buf_pool = buf_pool_get(cur_page_id);
		buf_page_t * buf_page = buf_page_hash_get_low(page_buf_pool, cur_page_id);

		if (buf_page != NULL) { /* 현재 다른 버퍼에 페이지가 존재한다면*/
			continue;
		}

		fil_space_t*		space	= fil_space_get(cur_page_id.space());
		const page_size_t	page_size(space->flags);
		if(ppl_buf_read_page_background(cur_page_id, page_size, false, buf_pool)){
			// fprintf(stderr, "PPL_Cleanig: ppl_buf_page_read_in_area: read page_id(%lu, %lu), buf_pool[%d]: %p\n", 
			// 		cur_page_id.space(), cur_page_id.page_no(), buf_pool->instance_no, buf_pool);
			n++;
		}
		
	}
	// fprintf(stderr, "ppl_buf_page_read_in_area: read_page_no: %u\n", n);
	return(n);
}

void
ppl_buf_flush_note_modification(
/*=============================*/
	buf_block_t*	block)	/*!< in: end lsn of the last mtr in the
					set of mtr's */
{
#ifdef UNIV_DEBUG
	{
		ut_ad(!srv_read_only_mode);
		ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
		ut_ad(block->page.buf_fix_count > 0);

		buf_pool_t*	buf_pool = buf_pool_from_block(block);

		ut_ad(!buf_pool_mutex_own(buf_pool));
		ut_ad(!buf_flush_list_mutex_own(buf_pool));

		ut_ad(start_lsn != 0);
		ut_ad(block->page.newest_modification <= end_lsn);
	}
#endif /* UNIV_DEBUG */
	// fprintf(stderr, "ppl_buf_flush_note_modification: block(%u, %u)\n", block->page.id.space(), block->page.id.page_no());
	block->page.newest_modification = log_sys->lsn;

	if (!block->page.oldest_modification) {
		buf_pool_t*	buf_pool = buf_pool_from_block(block);
		buf_flush_insert_into_flush_list(
			buf_pool, block, log_sys->lsn);
		// fprintf(stderr, "PPL_Cleanig: ppl_buf_flush_note_modification[%d]: buf_pool:%p block(%u, %u): %p FLUSH_LIST_LEN: %lu\n", 
		// 		buf_pool->instance_no, buf_pool, block->page.id.space(), block->page.id.page_no(), block, UT_LIST_GET_LEN(buf_pool->flush_list));
	} else {
		ut_ad(block->page.oldest_modification <= start_lsn);
	}

}

#endif
