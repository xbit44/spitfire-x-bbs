// Frozen beta.5 migration ledger, independently reconciled by DR. Never derive
// these immutable predecessor expectations from the current implementation.
void check_frozen_beta5_ledger(internal::SqliteConnection& connection){
    struct Row {int version; const char* name; const char* hash;};
    const Row expected[]={
        {1,"sfx-v1-initial-persistence-baseline","c9ed2791ec70d18cb69696f837d27cc74f931d7c3253247768b7245bf0864806"},
        {2,"sfx-caller-journal-foundation","945a1168d77bd572ca5d4d6e8495c087939f0611655c811b80e074329cdb5ae8"},
        {3,"sfx-menu-preferences","604e67cf60150fb507f5bc375cea3aa853bb5fc9af02ee6061ded5bfd55db0ec"},
        {4,"sfx-caller-utilities","85e381681e9c6321e32402c3fbac7ca77f0f194bd853e41371f2937f56b11db6"},
        {5,"sfx-classic-information","2fec7f2ea63d06408b31fb9945ffdb8d970643f5f75937cc6ad471af799cfa2b"},
        {6,"sfx-classic-caller-delete","b75db5e25705e24ff348561881a725d933be130b345d6f080984d592e3e603f5"},
        {7,"sfx-other-bbs-text","470af51af74fb33c27a66cb07dda456a457ca7f5b915260361d628583751ce4c"},
        {8,"sfx-core-local-messaging","8d9f2e17038ee7fd8946fede9fbe631cb162c361712dfdb5f23753be19229ff2"},
        {9,"sfx-echo-administration","b65b93acf575086aa74580a340d8f1a8d04f9911f7ad09354f7a8b01ff9b18ab"},
        {10,"sfx-empty-group-deletion","b916289d0a9a2ef00d47942b335939fc704e564efc97a26045c7ba24a82dc321"},
        {11,"sfx-individual-message-deletion","dc98e509581ab3859058c9602d591feea3769e5bc9d520f6853bf32add595617"},
        {12,"section8b_message_sources_and_pipe_policy","6f74615026f8eb09c3688731e68e3cc676faf0c94a869fad3fb065ebff0a317b"},
        {13,"section8c_message_retention_and_maintenance","e2d54f247b998e62ca02330ca46e92c40d9dd788f1e8d579f281c340acb9dfbd"},
        {14,"section8d_qwk_settings_continuation_and_atomic_receipts","88cdcb1dc59ae184b12ba821de2fa390f4660552fb8a3c14a1328e3047892cef"},
        {15,"section9_native_file_catalog_and_transfer_receipts","05936368b8af052eeee78f0ceb0fc27161a5b8df63061bd05924c33c131dcc77"},
        {16,"section11_outbound_door_catalog","d1bbb9c6a262fc28dd75fbc6fcb840b9944297e94c5ac1694de151115c91ea00"},
        {17,"section11_door_requirements","20dd63a28c5a04313896100dab27d735c86c1d9aaa3e5b26d3cbf1a56add6657"},
        {18,"section11_local_door_programs","552ca0ad52aedf81fa26774735dc85ddd1d93f3e7e1a1c6f3881c824f611cf6f"},
        {19,"section12_chat_settings","144ad40cec72d4d1830110689566d659c52bd327befdc3ced4982abbfac11568"},
        {20,"section12_private_chat_limits","c21b269a9b75c0db994941bce8d46b7a3a870835b040c360c222d3ee6e99eb12"},
        {21,"section14_ftn_configuration","36113aa220dc1d6770f1efa7a00403a2de65743fa3dc56e6be256b87fdb18e17"},
        {22,"section14_ftn_mail","e7afe1d2e499ce37250f3114ff9df87200fdd10e67c7d2706e6d2ab08df68f42"},
        {23,"optional_message_subject","ab93e60b25e6f074784d96948e9c37e38f789107150da86583c0b4d17fb72979"},
        {24,"file_group_deletion","a3e42dcf58a72224f83afefc3eba81ecc5fd2d1b8cd0cad5bf06ef9f1ec92590"},
        {25,"ftn_wire_body","3feec1d5d2a7a87ea2ca9fa8d73eefc88575b8e4d5c0c89ce1510f975aadfed0"},
        {26,"ftn_complete_subject","719ba10fbcedf3764962cfcc0a888069903389aa02648e4d73485171e913baff"},
        {27,"caller_message_location","73e558acfd94ac52866febd1833a60d967631642dc5a6ee838e93fb9d0af6013"},
        {28,"external_binkd_handoff","22463988ce278c25b7a3f8b3c9c626c0587be821234f3cdd88a8b8a95df6a90f"},
        {29,"ftn_history_recovery","c25dd2cccd27a463031931cef546ba9e11d466d1ee88487d30ddf5215200aadc"},
    };
    for(const auto& row:expected){
        internal::SqliteStatement q(connection.get(),"SELECT count(*) FROM sfx_schema_migrations WHERE version=?1 AND name=?2 AND content_sha256=?3","frozen beta5 ledger identity");
        q.bind(1,static_cast<std::int64_t>(row.version));q.bind(2,std::string_view(row.name));q.bind(3,std::string_view(row.hash));
        check(q.row() && q.int64(0)==1,"exact frozen beta5 migration version/name/hash retained");
    }
}
