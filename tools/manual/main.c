#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "data/format/scanner/scanner.h"
#include "regex/regex.h"
#include "memory/memory.h"

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

string_slice seek_section = {};

regex_handle annotation_regex = {};

string_slice slice_subrange(string_slice original, range_t range){
    if (range.start > original.length || range.start + range.size > original.length) return (string_slice){};
    return (string_slice){.data = original.data + range.start, .length = range.size};
}

bool print_annotation(regex_result result){
    if (!result.found || result.capture_count < 2) return true;
    string_slice categories = slice_subrange(result.full_slice,result.capture_groups[2]);
    string_slice content = slice_subrange(result.full_slice,result.capture_groups[1]);
    if (slices_equal(categories, seek_section, true)){
        print("%v",content);
    } else {
        string_splitter splitter = make_string_splitter_slice(categories, '/', false);
        while (string_splitter_advance(&splitter)){
            string_slice current = string_splitter_get_current(&splitter);
            if (current.length && (current.data[current.length-1]) == ')') current.length--;
            if (slices_equal(seek_section,current, true)){
                print("%v",content);
                break;
            }
        }
    }
    return true;
}

void print_section(int size, string_slice title, string_slice content){
    if (!seek_section.length) return;

    if (size == 1){
        print("===%v===",title);
    }
    
    if (slices_equal(title, seek_section, true)){
        print("%v",content);
    } else {
        regex_find_many(&annotation_regex, content, print_annotation);
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

    annotation_regex = init_regex("!\\[(^\\]*)\\]\\((^\\)*)\\)");

    if (argc > 1){
        seek_section = slice_from_literal(argv[1]);
    } else seek_section = slice_from_literal("overview");

    print("Printing system %s information",seek_section);
    
    traverse_directory("/docs", true, find_page_file);

    return 0;
}