#include "entitlements.h"
#include "utils/package_info.h"
#include "files/helpers.h"
#include "authorize.h"

#define PROC_ENT_PREF "com.redos.process"

#define ENTITLEMENT_DEBUG
#ifdef ENTITLEMENT_DEBUG
#define ent_print(...) print(__VA_ARGS__)
#else
#define ent_print(...)
#endif

void process_grant_entitlements(process_t *proc){
    if (!proc->bundle) return;//TODO: CLI tools don't have access to entitlements
    
    string path = string_format("%s/package.info",proc->bundle);
    string_slice package = read_file_slice(path.data);
    
    package_info info = parse_package_info(package.data);
    
    if (!info.entitlements) return;
    
    size_t ents = chunk_array_count(info.entitlements);
    auth_token tok = auth_get_proc_token(proc);
    
    for (u64 i = 0; i < ents; i++){
        string_slice ent = slice_from_string(CHUNK_ARRAY_GET(string, info.entitlements, i));
        if (slice_starts_with(ent, SLICE(PROC_ENT_PREF))){
            ent.data += sizeof(PROC_ENT_PREF)-1;
            ent.length -= sizeof(PROC_ENT_PREF)-1;
            if (ent.length && ent.data[0] == '.'){
                ent.data++;
                ent.length--;
            }
            ent_print("[ENTITLEMENT debug] found process entitlement %v",ent);
            if (slice_lit_match(ent, "signal", true)) auth_entitlement(tok, auth_process_send_signals, auth_resource_process);
            if (slice_lit_match(ent, "exceptions", true)) auth_entitlement(tok, auth_process_intercept_exceptions, auth_resource_process);
            if (slice_lit_match(ent, "input", true)) auth_entitlement(tok, auth_process_input, auth_resource_process);
            if (slice_lit_match(ent, "output", true)) auth_entitlement(tok, auth_process_output, auth_resource_process);
            if (slice_lit_match(ent, "info", true)) auth_entitlement(tok, auth_process_info, auth_resource_process);
            if (slice_lit_match(ent, "state", true)) auth_entitlement(tok, auth_process_state, auth_resource_process);
        } else print("[ENTITLEMENT error] Unknown entitlement %v",ent);
    }   
}