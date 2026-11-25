std::vector<SfxBaselineSchemaObject> file_manifest() {
    auto result = qwk_manifest();
    sqlite3* raw = nullptr;
    if (sqlite3_open(":memory:", &raw) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        throw std::runtime_error("File manifest database");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
    const auto execute = [&](const std::string& sql) {
        if (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("File manifest DDL");
    };
    execute("PRAGMA foreign_keys=OFF"); execute(proposed_migration_sql);
    for (const auto* migration : {&sfx_caller_journal_migration(), &sfx_menu_preferences_migration(),
        &sfx_caller_utilities_migration(), &sfx_classic_information_migration(), &sfx_classic_delete_migration(),
        &sfx_other_board_text_migration(), &sfx_local_message_migration(), &sfx_message_admin_migration(),
        &sfx_message_group_migration(), &sfx_message_delete_migration(), &sfx_message_source_migration(),
        &sfx_message_maintenance_migration(), &sfx_qwk_migration()}) execute(migration->sql);
    execute(file_migration_sql);
    std::vector<SfxBaselineSchemaObject> changed;
    if (!collect_schema_manifest(raw, &changed)) throw std::runtime_error("File manifest collection");
    for (const auto& object : changed) if (object.name.rfind("sfx_file_", 0) == 0) result.push_back(object);
    return result;
}
