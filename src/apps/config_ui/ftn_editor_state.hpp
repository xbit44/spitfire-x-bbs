#pragma once
#include <windows.h>
#include <commctrl.h>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include "sfx/application/ftn_storage.hpp"
#include "sfx/application/ftn_held_control.hpp"
#include "sfx/application/ftn_echo_import.hpp"
#include "sfx/application/ftn_setup.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/ftn_echo_list_download.hpp"
enum FtnControlId {
    IDC_FTN_LIST=11000,IDC_FTN_ADD,IDC_FTN_EDIT,IDC_FTN_RELOAD,IDC_FTN_SETUP,
    IDC_FTN_MODE,IDC_FTN_PROGRAM,IDC_FTN_PROGRAM_BROWSE,IDC_FTN_LISTENER,
    IDC_FTN_BIND,IDC_FTN_PORT,IDC_FTN_DIRECT,IDC_FTN_SETTINGS_SAVE,
    IDC_FTN_NAME,IDC_FTN_ADDRESS,IDC_FTN_ALIASES,IDC_FTN_ORIGIN,IDC_FTN_URL,
    IDC_FTN_ENABLED,IDC_FTN_KEEP,IDC_FTN_DAYS,IDC_FTN_SAVE,IDC_FTN_LATER,
    IDC_FTN_BACK,IDC_FTN_CONNECTIONS,IDC_FTN_ECHOES,IDC_FTN_FILES,IDC_FTN_ROUTES,
    IDC_FTN_LINK_LIST,IDC_FTN_LINK_ADD,IDC_FTN_LINK_EDIT,IDC_FTN_LINK_TABS,IDC_FTN_LINK_NAME,
    IDC_FTN_LINK_ADDRESS,IDC_FTN_LINK_USE,IDC_FTN_LINK_HOST,IDC_FTN_LINK_PORT,
    IDC_FTN_LINK_ENABLED,IDC_FTN_LINK_TLS,IDC_FTN_LINK_PLAIN,IDC_FTN_LINK_POLL,
    IDC_FTN_LINK_AREAFIX,IDC_FTN_LINK_FORWARD,IDC_FTN_LINK_SAVE,IDC_FTN_LINK_BACK,
    IDC_FTN_SESSION_SECRET,IDC_FTN_PACKET_SECRET,IDC_FTN_AREAFIX_SECRET,IDC_FTN_TIC_SECRET,
    IDC_FTN_SESSION_CLEAR,IDC_FTN_PACKET_CLEAR,IDC_FTN_AREAFIX_CLEAR,IDC_FTN_TIC_CLEAR,
    IDC_FTN_ECHO_LIST,IDC_FTN_ECHO_ADD,IDC_FTN_ECHO_EDIT,IDC_FTN_ECHO_IMPORT,
    IDC_FTN_ECHO_TAG,IDC_FTN_ECHO_DESCRIPTION,IDC_FTN_ECHO_LOCAL_NAME,
    IDC_FTN_ECHO_STORE,IDC_FTN_ECHO_GROUP,IDC_FTN_ECHO_LOCAL,IDC_FTN_ECHO_ACCESS,
    IDC_FTN_ECHO_READ,IDC_FTN_ECHO_POST,IDC_FTN_ECHO_REAL_NAME,IDC_FTN_ECHO_SAVE,
    IDC_FTN_FILE_LIST,IDC_FTN_FILE_ADD,IDC_FTN_FILE_EDIT,IDC_FTN_FILE_TAG,
    IDC_FTN_FILE_DESCRIPTION,IDC_FTN_FILE_AREA,IDC_FTN_FILE_SAVE,
    IDC_FTN_ROUTE_LIST,IDC_FTN_ROUTE_ADD,IDC_FTN_ROUTE_ADDRESS,IDC_FTN_ROUTE_VIA,
    IDC_FTN_ROUTE_FLAVOR,IDC_FTN_ROUTE_SAVE,
    IDC_FTN_DETAIL,IDC_FTN_IMPORT_SOURCE,IDC_FTN_IMPORT_BROWSE,IDC_FTN_IMPORT_LOAD,
    IDC_FTN_IMPORT_SEARCH,IDC_FTN_IMPORT_SELECT,IDC_FTN_IMPORT_CLEAR,IDC_FTN_IMPORT_APPLY,
    IDC_FTN_IMPORT_GROUP,IDC_FTN_IMPORT_NEW_GROUP,IDC_FTN_IMPORT_RENAME,
    IDC_FTN_IMPORT_REVIEW,IDC_FTN_IMPORT_BACK,IDC_FTN_IMPORT_CANCEL,
    IDC_FTN_REMOVE,IDC_FTN_PRESET,IDC_FTN_NETWORK_REVIEW,IDC_FTN_SETUP_CANCEL,
    IDC_FTN_LINK_ALIASES,IDC_FTN_LINK_TLS_NAME,IDC_FTN_LINK_CA,IDC_FTN_LINK_PROVIDER,
    IDC_FTN_LINK_REMOVE,IDC_FTN_PERMISSIONS,IDC_FTN_PERMISSION_LINK,
    IDC_FTN_PERMISSION_LIST,IDC_FTN_PERMISSION_RECEIVE,IDC_FTN_PERMISSION_SEND,
    IDC_FTN_PERMISSION_MANAGE,IDC_FTN_PERMISSION_APPLY,IDC_FTN_PERMISSION_BACK,
    IDC_FTN_ECHO_PIPE,IDC_FTN_ECHO_FIND,IDC_FTN_ECHO_BACK,IDC_FTN_ECHO_REMOVE,
    IDC_FTN_FILE_BACK,IDC_FTN_FILE_REMOVE,IDC_FTN_ROUTE_EDIT,IDC_FTN_ROUTE_REMOVE,
    IDC_FTN_ROUTE_BACK,IDC_FTN_IMPORT_ROWS,IDC_FTN_IMPORT_PATH,IDC_FTN_IMPORT_ENCODING,
    IDC_FTN_IMPORT_ACCESS,IDC_FTN_IMPORT_READ,IDC_FTN_IMPORT_POST,IDC_FTN_IMPORT_REQUIREMENTS,
    IDC_FTN_REVIEW_TEXT,IDC_FTN_REVIEW_ENABLE,IDC_FTN_REVIEW_FINISH,IDC_FTN_REVIEW_BACK,
    IDC_FTN_TLS_CERTIFICATE,IDC_FTN_TLS_KEY,IDC_FTN_HELD,IDC_FTN_IMPORT_PREVIOUS,IDC_FTN_IMPORT_NEXT,IDC_FTN_RECONCILE,IDC_FTN_NETWORK_DESCRIPTION,IDC_FTN_TLS_LISTENER,IDC_FTN_TLS_PORT,
    IDC_FTN_HELD_NETWORK,IDC_FTN_HELD_REASON,IDC_FTN_HELD_FIND,IDC_FTN_HELD_REFRESH,IDC_FTN_HELD_ROWS,IDC_FTN_HELD_DETAILS,IDC_FTN_HELD_NEXT,IDC_FTN_HELD_TEXT,IDC_FTN_HELD_MESSAGES,IDC_FTN_HELD_MESSAGE_NEXT,
    IDC_FTN_HELD_ACCOUNT_FIND,IDC_FTN_HELD_ACCOUNT_SEARCH,IDC_FTN_HELD_ACCOUNTS,IDC_FTN_HELD_ACCOUNT_NEXT,IDC_FTN_HELD_RESOLVE,IDC_FTN_HELD_REPROCESS,IDC_FTN_HELD_CHECK,IDC_FTN_MSG_ENABLE,IDC_FTN_MSG_DIRECTORY,IDC_FTN_MSG_BROWSE,
    IDC_NS_TITLE,IDC_NS_NEXT,IDC_NS_BACK,IDC_NS_CANCEL,IDC_NS_NAME,IDC_NS_PRESET,IDC_NS_ADDRESS,IDC_NS_GROUP,IDC_NS_GROUP_NAME,
    IDC_NS_HUB,IDC_NS_HUB_NAME,IDC_NS_HUB_ADDRESS,IDC_NS_HOST,IDC_NS_PORT,IDC_NS_SECRET0,IDC_NS_SECRET1,IDC_NS_SECRET2,IDC_NS_SECRET3,
    IDC_NS_SOURCE,IDC_NS_PATH,IDC_NS_BROWSE,IDC_NS_LOAD,IDC_NS_SEARCH,IDC_NS_ROWS,IDC_NS_COUNTS,IDC_NS_TAG,IDC_NS_ECHO_NAME,IDC_NS_ADD,IDC_NS_ALL,IDC_NS_NONE,
    IDC_NS_ACCESS,IDC_NS_READ,IDC_NS_POST,IDC_NS_APPLY,IDC_NS_APPLY_ALL,IDC_NS_RECEIVE,IDC_NS_SEND,IDC_NS_PREPARE,
    IDC_NS_ROUTE,IDC_NS_CONTACT,IDC_NS_TIMER,IDC_NS_MINUTES,IDC_NS_ORIGIN,IDC_NS_REAL,IDC_NS_ANSI,IDC_NS_PIPE,
    IDC_NS_MSG,IDC_NS_MSG_PATH,IDC_NS_MSG_BROWSE,IDC_NS_NODE_PATH,IDC_NS_NODE_BROWSE,IDC_NS_COUNT,IDC_NS_AGE,
    IDC_NS_ENABLE,IDC_NS_SAVE,IDC_NS_QUEUE,IDC_NS_CLONE,IDC_NS_CLONE_COUNT,IDC_NS_CLONE_AGE,IDC_NS_CLONE_ACCESS,IDC_NS_CLONE_READ,IDC_NS_CLONE_POST,
    IDC_NS_CLONE_PREVIEW,IDC_NS_CLONE_APPLY,IDC_NS_CLONE_ALL,IDC_NS_CLONE_CLEAR,IDC_NS_CLONE_BACK,
    IDC_NS_VALUE_COUNT,IDC_NS_VALUE_AGE,IDC_NS_VALUE_ACCESS,IDC_NS_VALUE_READ,IDC_NS_VALUE_POST,
    IDC_NS_ALL_REAL,IDC_NS_ALL_ANSI,IDC_NS_ALL_PIPE,IDC_NS_CLONE_SEARCH,IDC_NS_SUMMARY,IDC_NS_CALLER_NAME,
    IDC_FTN_NET_SOURCE,IDC_FTN_NET_FILE,IDC_FTN_NET_BROWSE,IDC_FTN_NODE_FILE,IDC_FTN_NODE_BROWSE,IDC_FTN_NET_MSG,IDC_FTN_NET_MSG_PATH,
    IDC_FTN_ECHO_ANSI,IDC_FTN_ECHO_COUNT,IDC_FTN_ECHO_AGE,IDC_FTN_ECHO_CLONE,IDC_FTN_LINK_CONTACT,IDC_FTN_IMPORT_RELOAD,
    IDC_FTN_DEFAULT_NETMAIL_HUB,IDC_FTN_LAST
};
struct FtnEditorState {
    std::optional<sfx::application::FtnConfiguration> config;
    std::optional<sfx::application::FtnConfiguration> draft;
    struct Pending{sfx::application::FtnTransferToken token;sfx::application::FtnConfiguration configuration;};std::optional<Pending> pending;
    bool draft_changed{},wizard{},enable_choice{};
    int network{-1},link{-1},echo{-1},file_echo{-1};
    bool new_network{},new_link{},new_echo{},new_file_echo{},populating{};
    HWND list{},mode{},program{},listener{},bind{},port{},direct{};
    HWND name{},address{},aliases{},origin{},url{},enabled{},keep{},days{},detail{},network_description{};
    HWND links{},link_name{},link_address{},link_use{},link_host{},link_port{},link_enabled{},link_tls{},link_plain{},link_poll{},link_areafix{},link_forward{};
    HWND link_tabs{};std::array<std::vector<HWND>,4> link_groups;
    std::array<HWND,4> secrets{},clear{};
    HWND echoes{},echo_tag{},echo_description{},echo_name{},echo_store{},echo_group{},echo_local{},echo_access{},echo_read{},echo_post{},echo_real_name{};
    HWND files{},file_tag{},file_description{},file_area{};
    HWND routes{},route_address{},route_via{},route_flavor{},default_netmail_hub{};
    int route{-1};
    HWND preset{},link_aliases{},link_tls_name{},link_ca{},link_provider{},echo_pipe{},echo_find{};
    HWND tls_certificate{},tls_key{},tls_listener{},tls_port{},review_text{},review_enable{};
    std::vector<sfx::application::MessageGroup> message_groups;
    std::vector<sfx::application::LocalEcho> local_echoes;
    std::vector<sfx::application::FileAreaConfiguration> file_areas;
    bool catalog_ready{};
    struct Setup {
        bool active{},clone{},clone_from_editor{},prepared{},edited{},receive{true},send{true},default_real{true};int step{1};
        std::uint64_t epoch{};std::string network,hub;
        std::array<std::vector<HWND>,11> groups;
        std::vector<HWND> legacy;
        sfx::application::FtnEchoList list;std::vector<std::size_t> visible;
        std::vector<std::string> clone_targets;std::optional<sfx::application::FtnEchoBulkOptions> clone_preview;
        std::vector<std::string> preview_targets;
        std::array<bool,10> uncollected{};
        struct Load {std::atomic<bool> cancel{},done{};std::thread thread;std::uint64_t epoch{};std::string network;bool local{};std::string path;sfx::platform::win32::FtnEchoListDownload result;std::optional<sfx::application::FtnEchoList> parsed;~Load(){cancel=true;if(thread.joinable())thread.join();}};
        std::shared_ptr<Load> load;std::vector<std::shared_ptr<Load>> retired;
    } setup;
    HWND net_source{},net_file{},node_file{},net_msg{},net_msg_path{},echo_ansi{},echo_count{},echo_age{},link_contact{};
    struct Navigation {HTREEITEM handle{};std::string network;int page{};};
    std::vector<Navigation> navigation;
    HTREEITEM held_item{};
    HWND import_source{},import_path{},import_encoding{},import_find{},import_rows{},import_count{},import_group_label{},import_rename{},import_access{},import_read{},import_post{};
    std::optional<sfx::application::FtnConfiguration> import_base;
    bool import_changed{},last_save_conflict{};
    sfx::application::FtnEchoList import_list;
    std::vector<std::size_t> import_visible;
    std::vector<sfx::application::FtnEchoImportRequirements> import_requirements;
    std::size_t import_offset{};
    struct ImportJob {std::atomic<bool> cancel{},done{};std::mutex mutex;std::thread thread;sfx::platform::win32::FtnEchoListDownload result;~ImportJob(){cancel=true;if(thread.joinable())thread.join();}};
    std::shared_ptr<ImportJob> import_job;
    HWND permission_links{},permission_rows{};int permission_link{-1};
    std::vector<std::pair<std::string,std::string>> permission_choices;
    struct PermissionRow{std::string tag;bool file{};};std::vector<PermissionRow> permission_items;
    HWND permission_receive{},permission_send{},permission_manage{};
    bool permission_edits{};
    HWND link_areafix_status{};std::array<HWND,4> secret_status{};
    struct Held {
        HWND network{},reason{},find{},rows{},text{},messages{},account_find{},accounts{};
        std::vector<std::string> network_codes,reason_codes;
        sfx::application::FtnHeldReviewQuery query;
        std::optional<sfx::application::FtnHeldReview> list,detail;
        sfx::application::FtnHeldRecipientQuery account_query;
        std::optional<sfx::application::FtnHeldRecipients> recipients;
        std::optional<sfx::application::FtnHeldControlRequest> pending;
    } held;
};
