
def find_log_type_count():
    f = open('/back_up/mysql-57-nvdimm-ipl/test_log/mysql_error_nvdimm.log', 'r')
    test_str = "type: "
    type_dict = {}
    while(True):
        now_line = f.readline()
        if(now_line == ''): 
            break
        if('type:' in now_line):
            type_num = int(now_line[len(test_str):-1])
            if(type_dict.get(type_num) == None):
                type_dict[type_num] = 0
            else:
                type_dict[type_num] += 1
        else:
            continue
    sorted_dict = dict(sorted(type_dict.items()))
    for log_type in sorted_dict.keys():
        print(log_type, ":", sorted_dict[log_type])

def find_max_log_size():
    f = open('/back_up/mysql-57-nvdimm-ipl/test_log/mysql_error_nvdimm.log', 'r')
    test_str = "actual_log_len: "
    max_length = 0
    while(True):
        now_line = f.readline()
        if(now_line == ''): 
            break
        if('actual_log_len:' in now_line):
            actual_log_len = int(now_line[len(test_str):-1])
            if(actual_log_len > max_length):
                max_length = actual_log_len
    print(max_length)
    
# find_max_log_size()
find_log_type_count()

