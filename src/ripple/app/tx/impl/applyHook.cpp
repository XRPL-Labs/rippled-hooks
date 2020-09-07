#include <ripple/app/tx/applyHook.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/Slice.h>
using namespace ripple;

TER
hook::setHookState(
    HookContext& hookCtx,
    Keylet const& hookStateKeylet,
    Slice& data
){

    auto& view = hookCtx.applyCtx.view();
    auto j = hookCtx.applyCtx.app.journal("View");
    auto const sle = view.peek(hookCtx.accountKeylet);
    if (!sle)
        return tefINTERNAL;

    auto const hook = view.peek(hookCtx.hookKeylet);
    if (!hook) { // [RH] should this be more than trace??
        JLOG(j.trace()) << "Attempted to set a hook state for a hook that doesnt exist " << toBase58(hookCtx.account);
        return tefINTERNAL;
    }

    uint32_t hookDataMax = hook->getFieldU32(sfHookDataMaxSize);

    // if the blob is too large don't set it
    if (data.size() > hookDataMax) {
       return temHOOK_DATA_TOO_LARGE; 
    } 

    uint32_t stateCount = hook->getFieldU32(sfHookStateCount);
    uint32_t oldStateReserve = COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount);

    auto const oldHookState = view.peek(hookStateKeylet);

    // if the blob is nil then delete the entry if it exists
    if (data.size() == 0) {
    
        if (!view.peek(hookStateKeylet))
            return tesSUCCESS; // a request to remove a non-existent entry is defined as success

        auto const hint = (*oldHookState)[sfOwnerNode];

        // Remove the node from the account directory.
        if (!view.dirRemove(hookCtx.ownerDirKeylet, hint, hookStateKeylet.key, false))
        {
            return tefBAD_LEDGER;
        }

        // remove the actual hook state obj
        view.erase(oldHookState);

        // adjust state object count
        if (stateCount > 0)
            --stateCount; // guard this because in the "impossible" event it is already 0 we'll wrap back to int_max

        // if removing this state entry would destroy the allotment then reduce the owner count
        if (COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount) < oldStateReserve)
            adjustOwnerCount(view, sle, -1, j);
        
        hook->setFieldU32(sfHookStateCount, COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount));

        return tesSUCCESS;
    }

    
    std::uint32_t ownerCount{(*sle)[sfOwnerCount]};

    if (oldHookState) { 
        view.erase(oldHookState);
    } else {

        ++stateCount;

        if (COMPUTE_HOOK_DATA_OWNER_COUNT(stateCount) > oldStateReserve) {
            // the hook used its allocated allotment of state entries for its previous ownercount
            // increment ownercount and give it another allotment
    
            ++ownerCount;
            XRPAmount const newReserve{
                view.fees().accountReserve(ownerCount)};

            if (STAmount((*sle)[sfBalance]).xrp() < newReserve)
                return tecINSUFFICIENT_RESERVE;

            
            adjustOwnerCount(view, sle, 1, j);

        }

        // update state count
        hook->setFieldU32(sfHookStateCount, stateCount);

    }

    // add new data to ledger
    auto newHookState = std::make_shared<SLE>(hookStateKeylet);
    view.insert(newHookState);
    newHookState->setFieldVL(sfHookData, data);

    if (!oldHookState) {
        // Add the hook to the account's directory if it wasn't there already
        auto const page = dirAdd(
            view,
            hookCtx.ownerDirKeylet,
            hookStateKeylet.key,
            false,
            describeOwnerDir(hookCtx.account),
            j);
        
        JLOG(j.trace()) << "Create/update hook state for account " << toBase58(hookCtx.account)
                     << ": " << (page ? "success" : "failure");
        
        if (!page)
            return tecDIR_FULL;

        newHookState->setFieldU64(sfOwnerNode, *page);

    }

    return tesSUCCESS;
}

void hook::printWasmerError()
{
  int error_len = wasmer_last_error_length();
  char *error_str = (char*)malloc(error_len);
  wasmer_last_error_message(error_str, error_len);
  printf("Error: `%s`\n", error_str);
    free(error_str);
}

TER hook::apply(Blob hook, ApplyContext& applyCtx, const AccountID& account) {

    wasmer_instance_t *instance = NULL;


    if (wasmer_instantiate(&instance, hook.data(), hook.size(), imports, imports_count) != wasmer_result_t::WASMER_OK) {
        printf("hook malformed\n");
        printWasmerError();
        return temMALFORMED;
    }


    HookContext hookCtx {
        .applyCtx = applyCtx,
        .account = account,
        .accountKeylet = keylet::account(account),
        .ownerDirKeylet = keylet::ownerDir(account),
        .hookKeylet = keylet::hook(account),
        .changedState = 
            std::make_shared<std::map<ripple::uint256 const, std::pair<bool, ripple::Blob>>>(),
        .exitType = hook_api::ExitType::ROLLBACK, // default is to rollback unless hook calls accept() or reject()
        .exitReason = std::string(""),
        .exitCode = -1
    };

    wasmer_instance_context_data_set ( instance, &hookCtx );
    printf("Set HookContext: %lx\n", (void*)&hookCtx);

    wasmer_value_t arguments[] = { { .tag = wasmer_value_tag::WASM_I64, .value = {.I64 = 0 } } };
    wasmer_value_t results[] = { { .tag = wasmer_value_tag::WASM_I64, .value = {.I64 = 0 } } };

    wasmer_instance_call(
        instance,
        "hook",
        arguments,
        1,
        results,
        1
    ); 
    
    
    /*!= wasmer_result_t::WASMER_OK) {
        printf("hook() call failed\n");
        printWasmerError();
        return temMALFORMED; /// todo: [RH] should be a hook execution error code tecHOOK_ERROR?
    }*/

    printf( "hook exit type was: %s\n", 
            ( hookCtx.exitType == hook_api::ExitType::ROLLBACK ? "ROLLBACK" : 
            ( hookCtx.exitType == hook_api::ExitType::ACCEPT ? "ACCEPT" : "REJECT" ) ) );

    printf( "hook exit reason was: `%s`\n", hookCtx.exitReason.c_str() );

    printf( "hook exit code was: %d\n", hookCtx.exitCode );

    if (hookCtx.exitType != hook_api::ExitType::ROLLBACK) {
        printf("Committing changes made by hook\n");
        commitChangesToLedger(hookCtx);
    }


    // todo: [RH] memory leak here, destroy the imports, instance using a smart pointer
    wasmer_instance_destroy(instance);
    printf("running hook 3\n");

    if (hookCtx.exitType == hook_api::ExitType::ACCEPT) {
        return tesSUCCESS;
    } else {
        return terNO_AUTH;
    }
}


int64_t hook_api::output_dbg ( wasmer_instance_context_t * wasm_ctx, uint32_t ptr, uint32_t len ) {

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx on current stack

    printf("HOOKAPI_output_dbg: ");
    if (len > 1024) len = 1024;
    for (int i = 0; i < len && i < memory_length; ++i)
        printf("%c", memory[ptr + i]);
    return len;

}
int64_t hook_api::set_state ( wasmer_instance_context_t * wasm_ctx, uint32_t key_ptr, uint32_t data_ptr_in, uint32_t in_len ) {

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (key_ptr + 32 > memory_length || data_ptr_in + hook::maxHookDataSize() > memory_length) {
        JLOG(j.trace())
            << "Hook tried to set_state using memory outside of the wasm instance limit";
        return OUT_OF_BOUNDS;
    }
    
    if (in_len == 0)
        return TOO_SMALL;

    auto const sle = view.peek(hookCtx.hookKeylet);
    if (!sle)
        return INTERNAL_ERROR;
    
    uint32_t maxSize = sle->getFieldU32(sfHookDataMaxSize); 
    if (in_len > maxSize)
        return TOO_BIG;
   
    ripple::uint256 key = ripple::uint256::fromVoid(memory + key_ptr);
    
    (*hookCtx.changedState)[key] =
        std::pair<bool, ripple::Blob> (true, 
                {memory + data_ptr_in,  memory + data_ptr_in + in_len});

    return in_len;

}


void hook::commitChangesToLedger ( HookContext& hookCtx ) {


    // first write all changes to state

    for (const auto& cacheEntry : *(hookCtx.changedState)) {
        bool is_modified = cacheEntry.second.first;
        const auto& key = cacheEntry.first;
        const auto& blob = cacheEntry.second.second;
        if (is_modified) {
            // this entry isn't just cached, it was actually modified
            auto HSKeylet = keylet::hook_state(hookCtx.account, key);
            auto slice = Slice(blob.data(), blob.size());
            setHookState(hookCtx, HSKeylet, slice); // should not fail because checks were done before map insertion
        }
    }

    // next write all output pseudotx
    // RH TODO ^
}



int64_t hook_api::get_state ( wasmer_instance_context_t * wasm_ctx, uint32_t key_ptr, uint32_t data_ptr_out, uint32_t out_len ) {

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (key_ptr + out_len > memory_length) {
        JLOG(j.trace())
            << "Hook tried to get_state using memory outside of the wasm instance limit";
        return OUT_OF_BOUNDS;
    }
   
    ripple::uint256 key= ripple::uint256::fromVoid(memory + key_ptr);

    // first check if the requested state was previously cached this session
    const auto& cacheEntry = hookCtx.changedState->find(key);
    if (cacheEntry != hookCtx.changedState->end())
        WRITE_WASM_MEMORY_AND_RETURN(
            data_ptr_out, out_len,
            cacheEntry->second.second.data(), cacheEntry->second.second.size(),
            memory, memory_length);

    // cache miss look it up
    auto const sle = view.peek(hookCtx.hookKeylet);
    if (!sle)
        return INTERNAL_ERROR;

    auto hsSLE = view.peek(keylet::hook_state(hookCtx.account, key));
    if (!hsSLE)
        return DOESNT_EXIST;
    
    Blob b = hsSLE->getFieldVL(sfHookData);

    // it exists add it to cache and return it
    hookCtx.changedState->emplace(key, std::pair<bool, ripple::Blob>(false, b));

    WRITE_WASM_MEMORY_AND_RETURN(
        data_ptr_out, out_len,
        b.data(), b.size(),
        memory, memory_length);
}



int64_t hook_api::accept     ( wasmer_instance_context_t * wasm_ctx, int32_t error_code, uint32_t data_ptr_in, uint32_t in_len ) {
    return hook_api::_exit(wasm_ctx, error_code, data_ptr_in, in_len, hook_api::ExitType::ACCEPT);
}
int64_t hook_api::reject     ( wasmer_instance_context_t * wasm_ctx, int32_t error_code, uint32_t data_ptr_in, uint32_t in_len ) {
    return hook_api::_exit(wasm_ctx, error_code, data_ptr_in, in_len, hook_api::ExitType::REJECT);
}
int64_t hook_api::rollback   ( wasmer_instance_context_t * wasm_ctx, int32_t error_code, uint32_t data_ptr_in, uint32_t in_len ) {
    return hook_api::_exit(wasm_ctx, error_code, data_ptr_in, in_len, hook_api::ExitType::ROLLBACK);
}

int64_t hook_api::_exit ( wasmer_instance_context_t * wasm_ctx, int32_t error_code, uint32_t data_ptr_in, uint32_t in_len, hook_api::ExitType exitType ) {

    HOOK_SETUP(); // populates memory_ctx, memory, memory_length, applyCtx, hookCtx on current stack

    if (data_ptr_in) {
        if (NOT_IN_BOUNDS(data_ptr_in, in_len)) {
            JLOG(j.trace())
                << "Hook tried to accept/reject/rollback but specified memory outside of the wasm instance limit when specifying a reason string";
            return OUT_OF_BOUNDS;
        }

        hookCtx.exitReason = std::string ( (const char*)(memory + data_ptr_in), (size_t)in_len  );
    }

    hookCtx.exitType = exitType;
    hookCtx.exitCode = error_code;

    wasmer_raise_runtime_error(0, 0);

    // unreachable
    return 0;

}
//int64_t reject      ( wasmer_instance_context_t * wasm_ctx, int32_t error_code, uint32_t data_ptr_out, uint32_t out_len );
//int64_t rollback    ( wasmer_instance_context_t * wasm_ctx, int32_t error_code, uint32_t data_ptr_out, uint32_t out_len );



/*int64_t hook_api::get_current_ledger_id ( wasmer_instance_context_t * wasm_ctx, uint32_t ptr ) {
    ripple::ApplyContext* applyCtx = (ripple::ApplyContext*) wasmer_instance_context_data_get(wasm_ctx);
    uint8_t *memory = wasmer_memory_data( wasmer_instance_context_memory(wasm_ctx, 0) );
}*/
