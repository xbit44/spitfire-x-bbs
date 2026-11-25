struct CompactionBudget {
    sqlite3* db;std::function<bool()> stop;std::chrono::steady_clock::time_point until;
    bool stopped{};
    CompactionBudget(sqlite3* value,std::function<bool()> callback):db(value),stop(std::move(callback)),until(std::chrono::steady_clock::now()+std::chrono::minutes(30)){
        sqlite3_progress_handler(db,1000,[](void* context)->int{auto& s=*static_cast<CompactionBudget*>(context);try{s.stopped=s.stopped || (s.stop && s.stop());}catch(...){s.stopped=true;}return s.stopped || std::chrono::steady_clock::now()>=s.until;},this);
    }
    ~CompactionBudget(){sqlite3_progress_handler(db,0,nullptr,nullptr);}
    void poll(){try{stopped=stopped || (stop && stop());}catch(...){stopped=true;}
        if(stopped || std::chrono::steady_clock::now()>=until)throw internal::StorageFailure(application::StorageErrorCode::busy,"compaction cancelled or deadline",SQLITE_INTERRUPT);}
};
std::array<std::uint8_t,32> database_logical_fingerprint(sqlite3* db,CompactionBudget* budget=nullptr){
    crypto_hash_sha256_state digest{};if(crypto_hash_sha256_init(&digest)!=0)throw internal::StorageFailure(application::StorageErrorCode::internal,"logical verification digest",SQLITE_ERROR);
    const auto raw=[&](const void* data,std::size_t size){if(size && crypto_hash_sha256_update(&digest,static_cast<const unsigned char*>(data),size)!=0)throw internal::StorageFailure(application::StorageErrorCode::internal,"logical verification digest",SQLITE_ERROR);};
    const auto number=[&](std::uint64_t value){std::array<unsigned char,8> bytes{};for(unsigned i=0;i<8;++i)bytes[i]=static_cast<unsigned char>(value>>(56-i*8));raw(bytes.data(),8);};
    const auto text=[&](std::string_view value){number(value.size());raw(value.data(),value.size());};
    const auto quoted=[](std::string_view value){std::string out="\"";for(const auto ch:value){if(ch=='\"')out+='\"';out+=ch;}return out+'\"';};
    internal::SqliteStatement tables(db,"SELECT name FROM sqlite_schema WHERE type='table' AND (name NOT GLOB 'sqlite_*' OR name='sqlite_sequence') ORDER BY name","verify all logical tables, not a subset of messages");
    while(tables.row()){
        if(budget)budget->poll();
        const auto table=tables.text(0);text(table);
        internal::SqliteStatement columns(db,("PRAGMA table_info("+quoted(table)+")").c_str(),"logical verification stable keys");
        std::vector<std::string> all;std::map<std::int64_t,std::string> primary;
        while(columns.row()){all.push_back(columns.text(1));if(columns.int64(5)>0)primary.emplace(columns.int64(5),columns.text(1));}
        if(all.empty())throw internal::StorageFailure(application::StorageErrorCode::corrupt,"logical verification columns missing",SQLITE_CORRUPT);
        std::string order;
        if(primary.empty())for(const auto& column:all){if(!order.empty())order+=',';order+=quoted(column);}
        else for(const auto& [index,column]:primary){(void)index;if(!order.empty())order+=',';order+=quoted(column);}
        internal::SqliteStatement rows(db,("SELECT * FROM "+quoted(table)+" ORDER BY "+order).c_str(),"compare every stored value without public content output");
        const auto count=sqlite3_column_count(rows.get());std::uint64_t row_count{};
        while(rows.row()){
            if(budget)budget->poll();
            ++row_count;number(static_cast<std::uint64_t>(count));
            for(int col=0;col<count;++col){const auto type=sqlite3_column_type(rows.get(),col);number(static_cast<std::uint64_t>(type));
                if(type==SQLITE_INTEGER)number(static_cast<std::uint64_t>(rows.int64(col)));
                else if(type==SQLITE_FLOAT){const double value=sqlite3_column_double(rows.get(),col);std::uint64_t bits{};std::memcpy(&bits,&value,sizeof(bits));number(bits);}
                else if(type==SQLITE_TEXT || type==SQLITE_BLOB){const auto size=sqlite3_column_bytes(rows.get(),col);number(static_cast<std::uint64_t>(size));raw(sqlite3_column_blob(rows.get(),col),static_cast<std::size_t>(size));}
            }
        }
        number(row_count);
    }
    std::array<std::uint8_t,32> result{};if(crypto_hash_sha256_final(&digest,result.data())!=0)throw internal::StorageFailure(application::StorageErrorCode::internal,"logical verification digest",SQLITE_ERROR);return result;
}
