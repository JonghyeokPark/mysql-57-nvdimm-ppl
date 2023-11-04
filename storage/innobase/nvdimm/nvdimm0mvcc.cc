#include "nvdimm-ipl.h"
#include "mtr0log.h"
#include "page0page.h"
#include "buf0flu.h"
#include <vector>

//lbh
//Redo based MVCC

void init_prebuilt_page_cache(std::vector<buf_page_t*> prebuilt_page_list){
    prebuilt_page_list.clear();
}

buf_page_t* add_prebuilt_page(buf_page_t* bpage){

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


void remove_prebuilt_page_from_list(buf_page_t* prebuilt_page, std::vector<buf_page_t*> prebuilt_page_list){  
    for(int idx=0; idx<prebuilt_page_list.size(); idx++){
        if(prebuilt_page_list[idx]==prebuilt_page){
            prebuilt_page_list.erase(prebuilt_page_list.begin()+idx);
        }
    }
}

buf_page_t* find_prebuilt_page_from_list(buf_page_t* prebuilt_page, std::vector<buf_page_t*> prebuilt_page_list){
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
