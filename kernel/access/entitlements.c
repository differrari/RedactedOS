#include "entitlements.h"
#include "files/helpers.h"
#include "authorize.h"

#define PROC_ENT_PREF "com.redos.process"

#define ENTITLEMENT_DEBUG
#ifdef ENTITLEMENT_DEBUG
#define ent_print(...) print(__VA_ARGS__)
#else
#define ent_print(...)
#endif

void process_grant_entitlements(process_t *proc, package_info info){
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
            if (slice_lit_match(ent, "signal", true)) auth_entitlement(tok, auth_process_send_signals, resource_process);
            if (slice_lit_match(ent, "exceptions", true)) auth_entitlement(tok, auth_process_intercept_exceptions, resource_process);
            if (slice_lit_match(ent, "input", true)) auth_entitlement(tok, auth_process_input, resource_process);
            if (slice_lit_match(ent, "output", true)) auth_entitlement(tok, auth_process_output, resource_process);
            if (slice_lit_match(ent, "info", true)) auth_entitlement(tok, auth_process_info, resource_process);
            if (slice_lit_match(ent, "state", true)) auth_entitlement(tok, auth_process_state, resource_process);
        } else print("[ENTITLEMENT error] Unknown entitlement %v",ent);
    }   
}