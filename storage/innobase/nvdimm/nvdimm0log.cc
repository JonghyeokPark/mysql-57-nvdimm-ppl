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

void nvdimm_ipl_initialize() {
	// TODO(jhpark): initialize IPL-related data structures 
	// For now, we use static allocation scheme.
	// We will support dynamic mapping for IPL region
	NVDIMM_INFO_PRINT("NVDIMM IPL Region Initialization!\n");
}

bool nvdimm_ipl_add(const page_id_t page_id, unsigned char *log, unsigned long len) {
	// debug 
	fprintf(stderr, "nvdimm_ip_add called! %lu:%lu\n", page_id.space(), page_id.page_no());
	// step1. get offset in NVDIMM IPL region from ipl_map table
	std::map<page_id_t, uint64_t>::iterator it;
	uint64_t ipl_start_offset = -1;
	uint64_t offset = -1;

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
	if (offset > 1024*1024*0.8) {
		std::cerr << "(debug) IPL page is FULL!\n";
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

bool nvdimm_ipl_merge(page_id_t page_id, buf_page_t * page) {
	// merge IPL log to buffer page
}


