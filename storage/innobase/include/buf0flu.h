/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/buf0flu.h
The database buffer pool flush algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#ifndef buf0flu_h
#define buf0flu_h

#include "univ.i"
#include "ut0byte.h"
#include "log0log.h"
#ifndef UNIV_HOTBACKUP
#include "buf0types.h"

/** Flag indicating if the page_cleaner is in active state. */
extern bool buf_page_cleaner_is_active;

#ifdef UNIV_DEBUG

/** Value of MySQL global variable used to disable page cleaner. */
extern my_bool		innodb_page_cleaner_disabled_debug;

#endif /* UNIV_DEBUG */

/** Event to synchronise with the flushing. */
extern os_event_t	buf_flush_event;

#ifdef UNIV_NVDIMM_PPL
extern bool ppl_buf_page_cleaner_is_active;
extern os_event_t	ppl_buf_flush_event;
#endif

class ut_stage_alter_t;

void
buf_flush_delete_from_flush_rbt(
/*============================*/
	buf_page_t*	bpage);

/********************************************************************//**
Remove a block from the flush list of modified blocks. */
void
buf_flush_remove(
/*=============*/
	buf_page_t*	bpage);	/*!< in: pointer to the block in question */
/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage has already been
copied to dpage. */
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage);	/*!< in/out: destination block */
/********************************************************************//**
Updates the flush system data structures when a write is completed. */
void
buf_flush_write_complete(
/*=====================*/
	buf_page_t*	bpage);	/*!< in: pointer to the block in question */
#endif /* !UNIV_HOTBACKUP */
/** Initialize a page for writing to the tablespace.
@param[in]	block		buffer block; NULL if bypassing the buffer pool
@param[in,out]	page		page frame
@param[in,out]	page_zip_	compressed page, or NULL if uncompressed
@param[in]	newest_lsn	newest modification LSN to the page
@param[in]	skip_checksum	whether to disable the page checksum */
void
buf_flush_init_for_writing(
	const buf_block_t*	block,
	byte*			page,
	void*			page_zip_,
	lsn_t			newest_lsn,
	bool			skip_checksum);

#ifndef UNIV_HOTBACKUP
# if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: buf_pool->mutex and block->mutex must be held upon entering this
function, and they will be released by this function after flushing.
This is loosely based on buf_flush_batch() and buf_flush_page().
@return TRUE if the page was flushed and the mutexes released */
ibool
buf_flush_page_try(
/*===============*/
	buf_pool_t*	buf_pool,	/*!< in/out: buffer pool instance */
	buf_block_t*	block)		/*!< in/out: buffer control block */
	MY_ATTRIBUTE((warn_unused_result));
# endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
/** Do flushing batch of a given type.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in,out]	buf_pool	buffer pool instance
@param[in]	type		flush type
@param[in]	min_n		wished minimum mumber of blocks flushed
(it is not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL
@retval true	if a batch was queued successfully.
@retval false	if another batch of same type was already running. */
bool
buf_flush_do_batch(
	buf_pool_t*		buf_pool,
	buf_flush_t		type,
	ulint			min_n,
	lsn_t			lsn_limit,
	ulint*			n_processed);

/** This utility flushes dirty blocks from the end of the flush list of all
buffer pool instances.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL.
@return true if a batch was queued successfully for each buffer pool
instance. false if another batch of same type was already running in
at least one of the buffer pool instance */
bool
buf_flush_lists(
	ulint			min_n,
	lsn_t			lsn_limit,
	ulint*			n_processed);

/******************************************************************//**
This function picks up a single page from the tail of the LRU
list, flushes it (if it is dirty), removes it from page_hash and LRU
list and puts it on the free list. It is called from user threads when
they are unable to find a replaceable page at the tail of the LRU
list i.e.: when the background LRU flushing in the page_cleaner thread
is not fast enough to keep pace with the workload.
@return true if success. */
bool
buf_flush_single_page_from_LRU(
/*===========================*/
	buf_pool_t*	buf_pool,
	ulint n_iterations);	/*!< in/out: buffer pool instance */
/******************************************************************//**
Waits until a flush batch of the given type ends */
void
buf_flush_wait_batch_end(
/*=====================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_flush_t	type);		/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
/**
Waits until a flush batch of the given lsn ends
@param[in]	new_oldest	target oldest_modified_lsn to wait for */

void
buf_flush_wait_flushed(
	lsn_t		new_oldest);

/******************************************************************//**
Waits until a flush batch of the given type ends. This is called by
a thread that only wants to wait for a flush to end but doesn't do
any flushing itself. */
void
buf_flush_wait_batch_end_wait_only(
/*===============================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_flush_t	type);		/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
/********************************************************************//**
This function should be called at a mini-transaction commit, if a page was
modified in it. Puts the block to the list of modified blocks, if it not
already in it. */

/********************************************************************//**
This function should be called when recovery has modified a buffer page. */
UNIV_INLINE
void
buf_flush_recv_note_modification(
/*=============================*/
	buf_block_t*	block,		/*!< in: block which is modified */
	lsn_t		start_lsn,	/*!< in: start lsn of the first mtr in a
					set of mtr's */
	lsn_t		end_lsn);	/*!< in: end lsn of the last mtr in the
					set of mtr's */
/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., transition FILE_PAGE => NOT_USED allowed.
@return TRUE if can replace immediately */
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage);	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */

#ifdef UNIV_DEBUG
/** Disables page cleaner threads (coordinator and workers).
It's used by: SET GLOBAL innodb_page_cleaner_disabled_debug = 1 (0).
@param[in]	thd		thread handle
@param[in]	var		pointer to system variable
@param[out]	var_ptr		where the formal string goes
@param[in]	save		immediate result from check function */
void
buf_flush_page_cleaner_disabled_debug_update(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				var_ptr,
	const void*			save);
#endif /* UNIV_DEBUG */

/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator of this thread.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(buf_flush_page_cleaner_coordinator)(
/*===============================================*/
	void*	arg);		/*!< in: a dummy parameter required by
				os_thread_create */
/******************************************************************//**
Worker thread of page_cleaner.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(buf_flush_page_cleaner_worker)(
/*==========================================*/
	void*	arg);		/*!< in: a dummy parameter required by
				os_thread_create */
/******************************************************************//**
Initialize page_cleaner. */
void
buf_flush_page_cleaner_init(void);

#ifdef UNIV_NVDIMM_PPL
/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator of this thread.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(ppl_buf_flush_page_cleaner_coordinator)(
/*===============================================*/
	void*	arg);		/*!< in: a dummy parameter required by
				os_thread_create */
/******************************************************************//**
Worker thread of ppl_cold_page_cleaner.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(buf_flush_ppl_page_cleaner_worker)(
/*==========================================*/
	void*	arg);
			/*!< in: a dummy parameter required by
			os_thread_create */
/******************************************************************//**
Initialize page_cleaner. */
void
buf_flush_ppl_page_cleaner_init(void);
#endif

/*=============================*/
/*********************************************************************//**
Clears up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return total pages flushed */
ulint
buf_flush_LRU_lists(void);
/*=====================*/
/*********************************************************************//**
Wait for any possible LRU flushes that are in progress to end. */
void
buf_flush_wait_LRU_batch_end(void);
/*==============================*/

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/******************************************************************//**
Validates the flush list.
@return TRUE if ok */
ibool
buf_flush_validate(
/*===============*/
	buf_pool_t*	buf_pool);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
void
buf_flush_init_flush_rbt(void);
/*==========================*/

/********************************************************************//**
Frees up the red-black tree. */
void
buf_flush_free_flush_rbt(void);
/*==========================*/

/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: in simulated aio we must call
os_aio_simulated_wake_handler_threads after we have posted a batch of
writes! NOTE: buf_pool->mutex and buf_page_get_mutex(bpage) must be
held upon entering this function, and they will be released by this
function.
@return TRUE if page was flushed */
bool
ipl_flush_page_and_try_neighbors(
	buf_page_t*		bpage,
	buf_flush_t		flush_type,
	ulint			n_to_flush,
	ulint*			count);
ulint
ipl_flush_try_neighbors(
	const page_id_t&	page_id,
	buf_flush_t		flush_type,
	ulint			n_flushed,
	ulint			n_to_flush);

ibool
buf_flush_ipl_clean_checkpointed_page(
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_page_t*	bpage,		/*!< in: buffer control block */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync);
void
buf_flush_ipl_clean_checkpointed_block_low(
/*======================*/
	buf_page_t*	bpage,		/*!< in: buffer block to write */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync);		/*!< in: true if sync IO request */

ibool
buf_flush_page(
/*===========*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_page_t*	bpage,		/*!< in: buffer control block */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync);		/*!< in: true if sync IO request */
/********************************************************************//**
Returns true if the block is modified and ready for flushing.
@return true if can flush immediately */
bool
buf_flush_ready_for_flush(
/*======================*/
	buf_page_t*	bpage,	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) */
	buf_flush_t	flush_type)/*!< in: type of flush */
	MY_ATTRIBUTE((warn_unused_result));

/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush
list in a particular buffer pool.
@return number of dirty pages present in a single buffer pool */
ulint
buf_pool_get_dirty_pages_count(
/*===========================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool */
	ulint		id,		/*!< in: space id to check */
	FlushObserver*	observer);	/*!< in: flush observer to check */
/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush list.
@return count of dirty pages present in all the buffer pools */
ulint
buf_flush_get_dirty_pages_count(
/*============================*/
	ulint		id,		/*!< in: space id to check */
	FlushObserver*	observer);	/*!< in: flush observer to check */

/*******************************************************************//**
Synchronously flush dirty blocks from the end of the flush list of all buffer
pool instances.
NOTE: The calling thread is not allowed to own any latches on pages! */
void
buf_flush_sync_all_buf_pools(void);
/*==============================*/

/** Request IO burst and wake page_cleaner up.
@param[in]	lsn_limit	upper limit of LSN to be flushed */
void
buf_flush_request_force(
	lsn_t	lsn_limit);

/** We use FlushObserver to track flushing of non-redo logged pages in bulk
create index(BtrBulk.cc).Since we disable redo logging during a index build,
we need to make sure that all dirty pages modifed by the index build are
flushed to disk before any redo logged operations go to the index. */

class FlushObserver {
public:
	/** Constructor
	@param[in]	space_id	table space id
	@param[in]	trx		trx instance
	@param[in]	stage		performance schema accounting object,
	used by ALTER TABLE. It is passed to log_preflush_pool_modified_pages()
	for accounting. */
	FlushObserver(ulint space_id, trx_t* trx, ut_stage_alter_t* stage);

	/** Deconstructor */
	~FlushObserver();

	/** Check pages have been flushed and removed from the flush list
	in a buffer pool instance.
	@pram[in]	instance_no	buffer pool instance no
	@return true if the pages were removed from the flush list */
	bool is_complete(ulint	instance_no)
	{
		return(m_flushed->at(instance_no) == m_removed->at(instance_no)
		       || m_interrupted);
	}

	/** Interrupt observer not to wait. */
	void interrupted()
	{
		m_interrupted = true;
	}

	/** Check whether trx is interrupted
	@return true if trx is interrupted */
	bool check_interrupted();

	/** Flush dirty pages. */
	void flush();

	/** Notify observer of flushing a page
	@param[in]	buf_pool	buffer pool instance
	@param[in]	bpage		buffer page to flush */
	void notify_flush(
		buf_pool_t*	buf_pool,
		buf_page_t*	bpage);

	/** Notify observer of removing a page from flush list
	@param[in]	buf_pool	buffer pool instance
	@param[in]	bpage		buffer page flushed */
	void notify_remove(
		buf_pool_t*	buf_pool,
		buf_page_t*	bpage);
private:
	/** Table space id */
	ulint			m_space_id;

	/** Trx instance */
	trx_t*			m_trx;

	/** Performance schema accounting object, used by ALTER TABLE.
	If not NULL, then stage->begin_phase_flush() will be called initially,
	specifying the number of pages to be attempted to be flushed and
	subsequently, stage->inc() will be called for each page we attempt to
	flush. */
	ut_stage_alter_t*	m_stage;

	/* Flush request sent */
	std::vector<ulint>*	m_flushed;

	/* Flush request finished */
	std::vector<ulint>*	m_removed;

	/* True if the operation was interrupted. */
	bool			m_interrupted;
};

#endif /* !UNIV_HOTBACKUP */

#ifndef UNIV_NONINL
#include "buf0flu.ic"
#endif

#endif
