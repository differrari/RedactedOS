#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "data/format/scanner/scanner.h"

void markdown_sections(string_slice document, void (*on_section)(int size, string_slice title, string_slice content)){
    Scanner scan = scanner_make(document.data,document.length);

    string_slice title = {};
    int count = 0;
    do {
        string_slice prev = scan_to(&scan, '#');

        int cur_count = (prev.length && prev.data[prev.length-1] == '#');

        if (title.length) on_section(count, slice_trim_ws(title,true), slice_trim_ws(cur_count ? (string_slice){prev.data,prev.length-1} : prev, true));
        if (!cur_count) break;
        count = cur_count;
        while (scan_next(&scan) == '#') count++;

        title = scan_to(&scan, '\n');

        // print("There are %i # in this title: %v",count,title);  
    } while(!scan_eof(&scan));
}

char *seek_section = 0;

void print_section(int size, string_slice title, string_slice content){
    if (!seek_section) return;

    if (size == 1){
        print("===%v===",title);
    }
    
    if (slice_lit_match(title, seek_section, true)){
        print("%v",content);
    }
}

void find_page_file(const char *directory, const char *file){

    if (!strncmp(file,".",1) || !strncmp(file,"..",2)) return;
    
    string s = string_format("%s/%s",directory,file);

    size_t length = 0;
    char *file_content = read_full_file(s.data, &length);

    markdown_sections((string_slice){file_content,length}, print_section);
    
    string_free(s);
}

int main(int argc, strarr argv){

    if (argc > 1){
        seek_section = argv[1];
    } else seek_section = "overview";

    print("Printing system %s information",seek_section);
    
    traverse_directory("/docs", true, find_page_file);

    return 0;
}