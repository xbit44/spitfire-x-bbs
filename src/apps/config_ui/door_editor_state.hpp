// Private SFXConfig state; no installed test catalog or cleartext secret fields.
enum DoorControlId {
    IDC_DOOR_VIEW=7000,IDC_DOOR_TREE,IDC_DOOR_MENU_ID,IDC_DOOR_MENU_NAME,IDC_DOOR_MENU_NEW,
    IDC_DOOR_MENU_SAVE,IDC_DOOR_MENU_REMOVE,IDC_DOOR_MENU_DUPLICATE,IDC_DOOR_MENU_DUP_ID,
    IDC_DOOR_ENTRIES,IDC_DOOR_ENTRY_NUMBER,IDC_DOOR_ENTRY_KIND,IDC_DOOR_ENTRY_TARGET,
    IDC_DOOR_ENTRY_TITLE,IDC_DOOR_ENTRY_DESCRIPTION,IDC_DOOR_ENTRY_SAVE,IDC_DOOR_ENTRY_REMOVE,
    IDC_DOOR_ENTRY_NEW,IDC_DOOR_COPY_MENU,IDC_DOOR_COPY_NUMBER,IDC_DOOR_ENTRY_COPY,
    IDC_DOOR_SELECT,IDC_DOOR_ID,IDC_DOOR_NAME,IDC_DOOR_ENABLED,IDC_DOOR_HOST,IDC_DOOR_PORT,
    IDC_DOOR_SERVER_MODE,IDC_DOOR_SERVER_NAME,IDC_DOOR_CLIENT_NAME,IDC_DOOR_TERMINAL,IDC_DOOR_AUTH,
    IDC_DOOR_SECRET,IDC_DOOR_PROMPT,IDC_DOOR_SECRET_CLEAR,IDC_DOOR_SALT,IDC_DOOR_PEPPER,
    IDC_DOOR_PEPPER_CLEAR,IDC_DOOR_TIMEOUT,IDC_DOOR_QUIET,IDC_DOOR_BANNER,IDC_DOOR_PAUSE,
    IDC_DOOR_SECURITY_MODE,IDC_DOOR_LEVEL,IDC_DOOR_SYSOP,IDC_DOOR_DETAIL,IDC_DOOR_SAVE,
    IDC_DOOR_NEW,IDC_DOOR_REMOVE,IDC_DOOR_DUPLICATE,IDC_DOOR_DUP_ID,IDC_DOOR_RELOAD,IDC_DOOR_RECONCILE,
    IDC_DOOR_OPEN,IDC_DOOR_BACK,IDC_DOOR_EDIT,IDC_DOOR_ADD_EXISTING,IDC_DOOR_PICK,
    IDC_DOOR_PICK_USE,IDC_DOOR_SETTINGS,IDC_DOOR_REMOTE,IDC_DOOR_CANCEL,
    IDC_DOOR_SHOW_ANOTHER,IDC_DOOR_DESTINATION,IDC_DOOR_DESTINATION_USE,IDC_DOOR_COMMAND_LINE,
    IDC_DOOR_ALL,IDC_DOOR_ALL_LIST,IDC_DOOR_CATEGORY_EDIT,IDC_DOOR_CATEGORY_PLACE,
    IDC_DOOR_CATEGORY_PARENT,IDC_DOOR_CATEGORY_NUMBER,IDC_DOOR_PLACEMENT_EDIT,
    IDC_DOOR_DUP_NAME,IDC_DOOR_CONFIRM,IDC_DOOR_RELOAD_DRAFT,
    IDC_DOOR_EXEC_MODE,IDC_DOOR_EXEC_LEVEL,IDC_DOOR_EXEC_SYSOP,
    IDC_DOOR_CAT_ACCESS_MODE,IDC_DOOR_CAT_ACCESS_LEVEL,IDC_DOOR_CAT_ACCESS_SYSOP,
    IDC_DOOR_CAT_EXEC_MODE,IDC_DOOR_CAT_EXEC_LEVEL,IDC_DOOR_CAT_EXEC_SYSOP,
    IDC_DOOR_FIND,IDC_DOOR_FIND_CLEAR,IDC_DOOR_ADVANCED,
    IDC_DOOR_TREE_CUT,IDC_DOOR_TREE_COPY,IDC_DOOR_TREE_PASTE,IDC_DOOR_TREE_DELETE,
    IDC_DOOR_TREE_INSERT,IDC_DOOR_TREE_PASTE_INTO,IDC_DOOR_TREE_ADD_DOOR,
    IDC_DOOR_DELETE_SELECTED,IDC_DOOR_DELETE_CURRENT,
    IDC_DOOR_TABS,IDC_DOOR_CODE,IDC_DOOR_STARTUP,IDC_DOOR_PROGRAM_TYPE,IDC_DOOR_IO,
    IDC_DOOR_SHELL,IDC_DOOR_DROP_FILE,IDC_DOOR_DROP_LOCATION,IDC_DOOR_MULTIPLE,
    IDC_DOOR_CLEANUP,IDC_DOOR_AUTO_EVENT,IDC_DOOR_EVENT_ONLY,IDC_DOOR_DROP_ENCODING,
    IDC_DOOR_TELNET_HOST,IDC_DOOR_TELNET_PORT,IDC_DOOR_TELNET_TERMINAL,IDC_DOOR_TELNET_TIMEOUT,IDC_DOOR_TELNET_APPLY,IDC_DOOR_BULLETINS,IDC_DOOR_LAST
};
enum class DoorPage : unsigned { categories, entries, settings, remote, existing, destination, held,
    definitions, category, placement, duplicate, confirm };
struct DoorEditorState {
    struct TreeItem {HTREEITEM handle{};app::DoorMenuEntry entry;bool unplaced{},group{};};
    std::optional<app::DoorCatalog> catalog;
    std::optional<app::DoorConfigurationChange> pending;
    // A normal Save can save the shared settings and then the menu entry using
    // the existing protected operations. Each step keeps its own exact receipt.
    std::vector<app::DoorConfigurationChange> save_steps;
    std::size_t save_step{};
    bool populating{},new_door{},place_new{},category_new{},category_place_new{},edit_placement{true},conflict{};
    DoorPage view{DoorPage::categories};
    DoorPage return_view{DoorPage::entries},back_view{DoorPage::categories};
    app::DoorMenuId category_id{};
    bool duplicate_category{};
    bool advanced{},clipboard_cut{},dragging{},open_created{},deleting_door{};
    std::vector<app::DoorConfigurationChange> confirmation_steps;
    HWND drag_source{},drag_surface{},drag_preview{},drag_marker{};
    std::optional<app::DoorMenuEntry> navigation_placement;
    std::optional<app::DoorMenuEntry> placement_entry;
    std::optional<TreeItem> drag_item,drag_drop;
    std::int64_t drag_revision{};POINT drag_anchor{};
    int drag_list_index{-1};ULONGLONG drag_scroll_at{},drag_hover_at{};
    unsigned drag_feedback_updates{};
    std::optional<TreeItem> clipboard;
    std::int64_t clipboard_revision{};
    std::vector<TreeItem> tree_items;
    std::vector<TreeItem> navigation_items;
    std::int64_t navigation_revision{-1};
    bool navigation_loading{};
    HTREEITEM drag_target{};int drag_position{};
    std::optional<app::DoorConfigurationChange> confirmation;
    app::DoorMenuEntry destination_entry;
    app::DoorMenuId menu{};app::DoorId door{};std::uint8_t entry{};
    HWND categories{},heading{},help{},error{},recovery{},entries{},entry_number{},entry_title{},entry_description{};
    std::array<HWND,2> action_detail{};
    HWND name{},command_line{},enabled{},host{},port{},server_mode{},server_name{},client_name{},terminal{},auth{},secret{},prompt{},secret_clear{},salt{},pepper{},pepper_clear{},timeout{},quiet{},banner{},pause{},security_mode{},level{},sysop{},detail{};
    HWND pick{},pick_number{},pick_title{},destination{},destination_number{};
    HWND definitions{},category_name{},category_parent{},category_number{},category_help{};
    HWND execution_mode{},execution_level{},execution_sysop{},find{},find_clear{},advanced_button{},credential_detail{},delete_selected{},delete_current{};
    HWND tabs{},code{},startup{},program_type{},io{},shell{},drop_file{},drop_location{},multiple{},cleanup{},automatic_event{},event_only{};
    HWND drop_encoding{},drop_encoding_label{};
    HWND telnet_host{},telnet_port{},telnet_terminal{},telnet_timeout{};
    bool telnet_helper_dirty{};
    std::vector<HWND> telnet_controls;
    unsigned selected_tab{};bool suggest_code{};
    std::array<std::vector<HWND>,4> tab_controls;
    std::vector<HWND> local_controls;
    std::array<HWND,6> category_requirements{};
    HWND placement_number{},placement_title{},placement_description{},duplicate_name{},confirm_text{},confirm_button{},confirm_cancel{},reload_draft{},back_settings{};
    std::array<std::vector<HWND>,12> controls;
    std::vector<HWND> settings_fields,existing_fields,destination_fields,category_fields,placement_fields,duplicate_fields,placement_controls,category_placement_controls,after_placement,advanced_controls,after_advanced;
    bool editing()const noexcept{return view==DoorPage::settings||view==DoorPage::remote;}
    bool draft_control(HWND h)const {
        const auto& fields=editing()?settings_fields:view==DoorPage::existing?existing_fields:view==DoorPage::category?category_fields:view==DoorPage::placement?placement_fields:view==DoorPage::duplicate?duplicate_fields:destination_fields;
        return (editing()||view==DoorPage::existing||view==DoorPage::destination||view==DoorPage::category||view==DoorPage::placement||view==DoorPage::duplicate)&&
            std::find(fields.begin(),fields.end(),h)!=fields.end();
    }
    int notice_offset(HWND h)const {
        const int compact=(!advanced&&std::find(after_advanced.begin(),after_advanced.end(),h)!=after_advanced.end()?-512:0);
        if(!pending&&!GetWindowTextLengthW(error)&&!conflict)return compact;
        for(const auto& group:controls)if(std::find(group.begin(),group.end(),h)!=group.end())return compact+64;
        return compact;
    }
};
