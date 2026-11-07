#include "container.h"
#include "syscalls/syscalls.h"

void setup_container(string_slice bundle_id){
    string s = string_format("/home/support/containers/%v",bundle_id);
    fs_stat out_stat = {};
    if (statf(s.data, &out_stat)){
        print("Bundle already has container");
    } else {
        string s1 = string_format("%S/config",s);
        create_directory(s1.data);
        string_free(s1);
        string s2 = string_format("%S/documents",s);
        create_directory(s2.data);
        string_free(s2);
    }
    //TODO: alias /config and /documents on the localfs to these folders
}