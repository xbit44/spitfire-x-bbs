#pragma once
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
namespace sfx::adapters::storage::internal {
inline application::FtnSerialState read_ftn_serial_state(sqlite3* db){
    SqliteStatement state(db,"SELECT high_water,recovery_required,(SELECT coalesce(max(serial),0) FROM sfx_ftn_serial_allocations),lineage FROM sfx_ftn_serial_allocator WHERE singleton_id=1","verify durable FTN counter and retained allocation floor");
    if(!state.row()||state.int64(0)<0||state.int64(0)>UINT32_MAX||state.int64(2)>state.int64(0)||(state.int64(1)!=0&&state.int64(1)!=1))throw StorageFailure(application::StorageErrorCode::corrupt,"FTN message ID history is unavailable",SQLITE_CORRUPT);
    const auto bytes=state.blob(3);application::Identifier128 lineage{};
    if(bytes.size()==lineage.bytes.size())std::copy(bytes.begin(),bytes.end(),lineage.bytes.begin());
    if(!lineage.valid())throw StorageFailure(application::StorageErrorCode::corrupt,"FTN allocation lineage is unavailable",SQLITE_CORRUPT);
    std::int64_t generation{};
    SqliteStatement version(db,"PRAGMA user_version","read FTN history schema");
    if(!version.row())throw StorageFailure(application::StorageErrorCode::corrupt,"FTN history schema unavailable",SQLITE_CORRUPT);
    if(version.int64(0)>=29){
        SqliteStatement recovery(db,"SELECT recovery_generation FROM sfx_ftn_serial_allocator WHERE singleton_id=1","read FTN recovery generation");
        if(!recovery.row()||(generation=recovery.int64(0))<0)throw StorageFailure(application::StorageErrorCode::corrupt,"FTN recovery generation unavailable",SQLITE_CORRUPT);
    }
    return {static_cast<std::uint32_t>(state.int64(0)),state.boolean(1),lineage,false,generation};
}
}
