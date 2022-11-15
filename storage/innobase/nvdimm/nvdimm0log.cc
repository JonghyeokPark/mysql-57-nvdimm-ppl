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

void nvdimm_ipl_initialize() {
	// TODO(jhpark): initialize IPL-related data structures 
	// For now, we use static allocation scheme.
	// We will support dynamic mapping for IPL region
	NVDIMM_INFO_PRINT("NVDIMM IPL Region Initialization!\n");
}

bool nvdimm_ipl_add(const page_id_t page_id, unsigned char *log, unsigned long len) {
	// step1. get offset in NVDIMM IPL region from ipl_map table
	std::map<page_id_t, uint64_t>::iterator it;
	uint64_t ipl_start_offset = -1;
	uint64_t offset = -1;

	if (nvdimm_offset >= (14*1024*1024*1024UL)) {
		std::cerr << "we need more ... NVDIMMM region\n";
		nvdimm_offset = 0;
	}

	it = ipl_map.find(page_id);
	if (it == ipl_map.end()) {
	// - First write
		ipl_map.insert(std::pair<page_id_t, uint64_t>(page_id, nvdimm_offset));
		ipl_wp.insert(std::pair<page_id_t, uint64_t>(page_id, 0));
		ipl_start_offset = nvdimm_offset;
		nvdimm_offset += IPL_LOG_REGION_SZ;
	} else {
	// - Get offset
		ipl_start_offset = it->second;
	}

	offset = ipl_wp[page_id];

	// step2. check capacity 
	if (offset > IPL_LOG_REGION_SZ*0.8) {
		//std::cerr << "(debug) IPL page is FULL!\n";
		return false;
	}

	// TODO(jhaprk): fix and reconsider ... Atomicity issues
	// log << header << persist(log) << payload << persist(log)
  // (jhpark) : header version brought from 
  //            [Persistent Memory I/O Primitives](https://arxiv.org/pdf/1904.01614.pdf)

	// step3. add mini transaction log to IPL table
	IPL_LOG_HDR log_hdr;
	log_hdr.flag = true;
	log_hdr.offset = offset;

	memcpy(nvdimm_ptr + ipl_start_offset, &log_hdr, sizeof(IPL_LOG_HDR));
	flush_cache(nvdimm_ptr + ipl_start_offset, sizeof(IPL_LOG_HDR));
	memcpy(nvdimm_ptr + ipl_start_offset + offset, log, len);
	flush_cache(nvdimm_ptr + ipl_start_offset + offset , len);

	// step5. updaet write pointer 
	offset += len;	
	ipl_wp[page_id] = offset;

	return true;
}

unsigned char* nvdimm_ipl_log_apply(page_id_t page_id, buf_block_t* block) {
	/*
		For example, we can utilize like this ... (see, log0recv.cc)
		recv = UT_LIST_GET_FIRST(recv_addr->rec_list);
		while (recv) {
			end_lsn = recv->end_lsn;
			buf = ((byte*)(recv->data)) + sizeof(recv_data_t);
			
			if (recv->start_lsn >= page_lsn
				&& !srv_is_tablespace_truncated(recv_addr->space)
				&& !skip_recv) {
			
				lsn_t end_lsn;
				recv_parse_or_apply_log_rec_body(
					recv->type, buf, buf+recv->len,
					recv_addr->space, recv_addr->page_no, block, &mtr);
			
				end_lsn = recv->start_lsn + recv->len;
				mach_write_to_8(FIL_PAGE_LSN + page, end_lsn);
				mach_write_to_8(UNIV_PAGE_SIZE
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ page, end_lsn);

				recv = UT_LIST_GET_NEXT(rec_list, recv);			
			}
		}
	*/

	mtr_t temp_mtr;
	mtr_start(&temp_mtr);

	// step1. read current IPL log using page_id
	uint64_t offset = ipl_map[page_id];
	uint64_t end_offset = ipl_wp[page_id];
	
	// step2. apply IPL Logs 
	// 전체 IPL log를 갖고 있는데.. 어떻게 처리 ?
	// buffer_end 까지 처리 할 수 있으므로, 우선은 parameter로 지급... 
	unsigned char* ptr = nvdimm_ptr + offset;
	unsigned char* end_ptr = nvdimm_ptr + end_offset;
	mlog_id_t type;
	unsigned long space, page_no;

	ib::info() << page_id.space() << ":" << page_id.page_no()  << " IPL applying start!";

	while (true) {
		// - get mtr log type ...
		unsigned char* new_ptr = mlog_parse_initial_log_record(ptr, end_ptr, &type, &space, &page_no);
		// debug 
		if (space != page_id.space() 
				|| page_no != page_id.page_no()) {
			fprintf(stderr, "[ERROR] IPL log parsing is wrong!\n");
		}

		new_ptr  = recv_parse_or_apply_log_rec_body(
										type, ptr, end_ptr, space, page_no, block, &temp_mtr);

		if ((new_ptr - ptr) == 0) break;
	}
	
	ib::info() << page_id.space() << ":" << page_id.page_no()  << " IPL applying finish!";
}

/*
unsigned char* nvdimm_ipl_log_apply( 
	mlog_id_t type, 
	unsigned char* ptr,
	unsigned char* end_ptr,
	unsigned long space,
	unsigned long page_no,
	buf_block_t* block) {
	
	mtr_t temp_mtr;	

	  switch (type) {
  case MLOG_FILE_NAME:
  case MLOG_FILE_DELETE:
  case MLOG_FILE_CREATE2:
  case MLOG_FILE_RENAME2:
    ut_ad(block == NULL);
    return(fil_name_parse(ptr, end_ptr, space_id, page_no, type));
  case MLOG_INDEX_LOAD:
#ifdef UNIV_HOTBACKUP
    if (!recv_recovery_on) {
      if (is_online_redo_copy) {
        if (backup_redo_log_flushed_lsn
            < recv_sys->recovered_lsn) {
          ib::trace() << "Last flushed lsn: "
            << backup_redo_log_flushed_lsn
            << " load_index lsn "
            << recv_sys->recovered_lsn;

          if (backup_redo_log_flushed_lsn == 0)
            ib::error() << "MEB was not "
              "able to determine the"
              "InnoDB Engine Status";

          ib::fatal() << "An optimized(without"
            " redo logging) DDLoperation"
            " has been performed. All"
            " modified pages may not have"
            " been flushed to the disk yet."
            " \n    MEB will not be able"
            " take a consistent backup."
            " Retry the backup operation";
        }
      } else {
        ib::trace() << "Last flushed lsn: "
          << backup_redo_log_flushed_lsn
          << " load_index lsn "
          << recv_sys->recovered_lsn;

        ib::warn() << "An optimized(without redo"
          " logging) DDL operation has been"
          " performed. All modified pages may not"
          " have been flushed to the disk yet."
          " \n    This offline backup may not"
          " be consistent";
      }
    }
#endif
    if (end_ptr < ptr + 8) {
      return(NULL);
    }
    return(ptr + 8);
  case MLOG_TRUNCATE:
    return(truncate_t::parse_redo_entry(ptr, end_ptr, space_id));
  case MLOG_WRITE_STRING:
    if (page_no == 0 && !is_system_tablespace(space_id)) {
      return(fil_write_encryption_parse(ptr,
                end_ptr,
                space_id));
    }
    break;

  default:
    break;
  }

  dict_index_t* index = NULL;
  page_t*   page;
  page_zip_des_t* page_zip;
#ifdef UNIV_DEBUG
  ulint   page_type;
#endif 
  page = block->frame;
  page_zip = buf_block_get_page_zip(block);
  ut_d(page_type = fil_page_get_type(page));
	
	const byte* old_ptr = ptr;

  switch (type) {
#ifdef UNIV_LOG_LSN_DEBUG
  case MLOG_LSN:
    break;
#endif 
  case MLOG_1BYTE: case MLOG_2BYTES: case MLOG_4BYTES: case MLOG_8BYTES:
#ifdef UNIV_DEBUG
    if (page && page_type == FIL_PAGE_TYPE_ALLOCATED
        && end_ptr >= ptr + 2) {
      ulint offs = mach_read_from_2(ptr);

      switch (type) {
      default:
        ut_error;
      case MLOG_2BYTES:
        ut_ad(offs == FIL_PAGE_TYPE
              || offs == IBUF_TREE_SEG_HEADER
              + IBUF_HEADER + FSEG_HDR_OFFSET
              || offs == PAGE_BTR_IBUF_FREE_LIST
              + PAGE_HEADER + FIL_ADDR_BYTE
              || offs == PAGE_BTR_IBUF_FREE_LIST
              + PAGE_HEADER + FIL_ADDR_BYTE
              + FIL_ADDR_SIZE
              || offs == PAGE_BTR_SEG_LEAF
              + PAGE_HEADER + FSEG_HDR_OFFSET
              || offs == PAGE_BTR_SEG_TOP
              + PAGE_HEADER + FSEG_HDR_OFFSET
              || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
              + PAGE_HEADER + FIL_ADDR_BYTE
              + 0 
              || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
              + PAGE_HEADER + FIL_ADDR_BYTE
              + FIL_ADDR_SIZE);
        break;
      case MLOG_4BYTES:
        ut_ad(0
              || offs == IBUF_TREE_SEG_HEADER
              + IBUF_HEADER + FSEG_HDR_SPACE
              || offs == IBUF_TREE_SEG_HEADER
              + IBUF_HEADER + FSEG_HDR_PAGE_NO
              || offs == PAGE_BTR_IBUF_FREE_LIST
              + PAGE_HEADER
              || offs == PAGE_BTR_IBUF_FREE_LIST
              + PAGE_HEADER + FIL_ADDR_PAGE
              || offs == PAGE_BTR_IBUF_FREE_LIST
              + PAGE_HEADER + FIL_ADDR_PAGE
              + FIL_ADDR_SIZE
              || offs == PAGE_BTR_SEG_LEAF
              + PAGE_HEADER + FSEG_HDR_PAGE_NO
              || offs == PAGE_BTR_SEG_LEAF
              + PAGE_HEADER + FSEG_HDR_SPACE
              || offs == PAGE_BTR_SEG_TOP
              + PAGE_HEADER + FSEG_HDR_PAGE_NO
              || offs == PAGE_BTR_SEG_TOP
              + PAGE_HEADER + FSEG_HDR_SPACE
              || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
              + PAGE_HEADER + FIL_ADDR_PAGE
              + 0 
              || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
              + PAGE_HEADER + FIL_ADDR_PAGE
              + FIL_ADDR_SIZE);
        break;
      }
    }
#endif 
    ptr = mlog_parse_nbytes(type, ptr, end_ptr, page, page_zip);
    if (ptr != NULL && page != NULL
        && page_no == 0 && type == MLOG_4BYTES) {
      ulint offs = mach_read_from_2(old_ptr);
      switch (offs) {
        fil_space_t*  space;
        ulint   val;
      default:
        break;
      case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
      case FSP_HEADER_OFFSET + FSP_SIZE:
      case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
      case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:
        space = fil_space_get(space_id);
        ut_a(space != NULL);
        val = mach_read_from_4(page + offs);

        switch (offs) {
        case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
          space->flags = val;
          break;
        case FSP_HEADER_OFFSET + FSP_SIZE:
          space->size_in_header = val;
          break;
        case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
          space->free_limit = val;
          break;
        case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:
          space->free_len = val;
          ut_ad(val == flst_get_len(
                  page + offs));
          break;
        }
      }
    }
    break;
 case MLOG_REC_INSERT: case MLOG_COMP_REC_INSERT:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type == MLOG_COMP_REC_INSERT,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = page_cur_parse_insert_rec(FALSE, ptr, end_ptr,
              block, index, mtr);
    }
    break;
  case MLOG_REC_CLUST_DELETE_MARK: case MLOG_COMP_REC_CLUST_DELETE_MARK:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type == MLOG_COMP_REC_CLUST_DELETE_MARK,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = btr_cur_parse_del_mark_set_clust_rec(
        ptr, end_ptr, page, page_zip, index);
    }
    break;
  case MLOG_COMP_REC_SEC_DELETE_MARK:
    ut_ad(!page || fil_page_type_is_index(page_type));
    ut_a(!page || page_is_comp(page));
    ut_a(!page_zip);
    ptr = mlog_parse_index(ptr, end_ptr, TRUE, &index);
    if (!ptr) {
      break;
    }
  case MLOG_REC_SEC_DELETE_MARK:
    ut_ad(!page || fil_page_type_is_index(page_type));
    ptr = btr_cur_parse_del_mark_set_sec_rec(ptr, end_ptr,
               page, page_zip);
    break;
  case MLOG_REC_UPDATE_IN_PLACE: case MLOG_COMP_REC_UPDATE_IN_PLACE:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type == MLOG_COMP_REC_UPDATE_IN_PLACE,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = btr_cur_parse_update_in_place(ptr, end_ptr, page,
                  page_zip, index);
    }
    break;
 case MLOG_LIST_END_DELETE: case MLOG_COMP_LIST_END_DELETE:
  case MLOG_LIST_START_DELETE: case MLOG_COMP_LIST_START_DELETE:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type == MLOG_COMP_LIST_END_DELETE
             || type == MLOG_COMP_LIST_START_DELETE,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = page_parse_delete_rec_list(type, ptr, end_ptr,
               block, index, mtr);
    }
    break;
  case MLOG_LIST_END_COPY_CREATED: case MLOG_COMP_LIST_END_COPY_CREATED:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type == MLOG_COMP_LIST_END_COPY_CREATED,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = page_parse_copy_rec_list_to_created_page(
        ptr, end_ptr, block, index, mtr);
    }
    break;
  case MLOG_PAGE_REORGANIZE:
  case MLOG_COMP_PAGE_REORGANIZE:
  case MLOG_ZIP_PAGE_REORGANIZE:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type != MLOG_PAGE_REORGANIZE,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = btr_parse_page_reorganize(
        ptr, end_ptr, index,
        type == MLOG_ZIP_PAGE_REORGANIZE,
        block, mtr);
    }
    break;
case MLOG_PAGE_CREATE: case MLOG_COMP_PAGE_CREATE:
    ut_a(!page_zip);
    page_parse_create(block, type == MLOG_COMP_PAGE_CREATE, false);
    break;
  case MLOG_PAGE_CREATE_RTREE: case MLOG_COMP_PAGE_CREATE_RTREE:
    page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_RTREE,
          true);
    break;
  case MLOG_UNDO_INSERT:
    ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
    ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page);
    break;
  case MLOG_UNDO_ERASE_END:
    ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
    ptr = trx_undo_parse_erase_page_end(ptr, end_ptr, page, mtr);
    break;
  case MLOG_UNDO_INIT:
    ptr = trx_undo_parse_page_init(ptr, end_ptr, page, mtr);
    break;
  case MLOG_UNDO_HDR_DISCARD:
    ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
    ptr = trx_undo_parse_discard_latest(ptr, end_ptr, page, mtr);
    break;
  case MLOG_UNDO_HDR_CREATE:
  case MLOG_UNDO_HDR_REUSE:
    ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
    ptr = trx_undo_parse_page_header(type, ptr, end_ptr,
             page, mtr);
    break;
  case MLOG_REC_MIN_MARK: case MLOG_COMP_REC_MIN_MARK:
    ut_ad(!page || fil_page_type_is_index(page_type));
    ut_a(type == MLOG_COMP_REC_MIN_MARK || !page_zip);
    ptr = btr_parse_set_min_rec_mark(
      ptr, end_ptr, type == MLOG_COMP_REC_MIN_MARK,
      page, mtr);
    break;
  case MLOG_REC_DELETE: case MLOG_COMP_REC_DELETE:
    ut_ad(!page || fil_page_type_is_index(page_type));

    if (NULL != (ptr = mlog_parse_index(
             ptr, end_ptr,
             type == MLOG_COMP_REC_DELETE,
             &index))) {
      ut_a(!page
           || (ibool)!!page_is_comp(page)
           == dict_table_is_comp(index->table));
      ptr = page_cur_parse_delete_rec(ptr, end_ptr,
              block, index, mtr);
    }
    break;
  case MLOG_IBUF_BITMAP_INIT:
    ptr = ibuf_parse_bitmap_init(ptr, end_ptr, block, mtr);
    break;
  case MLOG_INIT_FILE_PAGE:
  case MLOG_INIT_FILE_PAGE2:
    ptr = fsp_parse_init_file_page(ptr, end_ptr, block);
    break;
  case MLOG_WRITE_STRING:
    ut_ad(!page || page_type != FIL_PAGE_TYPE_ALLOCATED
          || page_no == 0);
    ptr = mlog_parse_string(ptr, end_ptr, page, page_zip);
    break;
  case MLOG_ZIP_WRITE_NODE_PTR:
    ut_ad(!page || fil_page_type_is_index(page_type));
    ptr = page_zip_parse_write_node_ptr(ptr, end_ptr,
                page, page_zip);
    break;
  case MLOG_ZIP_WRITE_BLOB_PTR:
    ut_ad(!page || fil_page_type_is_index(page_type));
    ptr = page_zip_parse_write_blob_ptr(ptr, end_ptr,
                page, page_zip);
    break;
  case MLOG_ZIP_WRITE_HEADER:
    ut_ad(!page || fil_page_type_is_index(page_type));
    ptr = page_zip_parse_write_header(ptr, end_ptr,
              page, page_zip);
    break;
  case MLOG_ZIP_PAGE_COMPRESS:
    ptr = page_zip_parse_compress(ptr, end_ptr,
                page, page_zip);
    break;
  case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
    if (NULL != (ptr = mlog_parse_index(
        ptr, end_ptr, TRUE, &index))) {

      ut_a(!page || ((ibool)!!page_is_comp(page)
        == dict_table_is_comp(index->table)));
      ptr = page_zip_parse_compress_no_data(
        ptr, end_ptr, page, page_zip, index);
    }
    break;
  default:
    ptr = NULL;
    recv_sys->found_corrupt_log = true;
  }

  if (index) {
    dict_table_t* table = index->table;

    dict_mem_index_free(index);
    dict_mem_table_free(table);
  }

  return(ptr);
}
*/

void nvdimm_ipl_erase(page_id_t page_id, buf_page_t* page) {
	// When the page is flushed, we need to delete IPL log from NVDIMM
	// step1. read current IPL log using page_id
	uint64_t offset = ipl_map[page_id];
	uint64_t end_offset = ipl_wp[page_id];
	
	// step2. delete IPL Logs 
	unsigned char* ptr = nvdimm_ptr + offset;
	unsigned char* end_ptr = nvdimm_ptr + end_offset;
	//ib::info() << page_id.space() << ":" << page_id.page_no()  << " IPL delete start!";
	memset(ptr, 0x00, IPL_LOG_REGION_SZ);
	flush_cache(ptr, IPL_LOG_REGION_SZ);
	ipl_wp[page_id] = 0;
	//ib::info() << page_id.space() << ":" << page_id.page_no()  << " IPL delete finish!";
	return;
}

bool nvdimm_ipl_lookup(page_id_t page_id) {
	// return true, 
	// if page exists in IPL region and IPL log is written
	
	return (ipl_map[page_id] && ipl_wp[page_id]!=0);
}

bool nvdimm_ipl_merge(page_id_t page_id, buf_page_t * page) {
	// merge IPL log to buffer page
}


