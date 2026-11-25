#pragma once
#include "sfx/application/door_bulletin.hpp"
#include "sfx/application/display.hpp"
namespace sfx::application {
// Same inert sample conventions as Displays & Menus. No credential, secret or
// active caller is loaded to preview an owner-selected display file.
inline TrustedDisplaySelectionRequest door_bulletin_preview_request(DoorBulletinId id,
    GraphicsMode graphics,std::string_view bbs,std::string_view version,unsigned nodes){
    TrustedDisplaySelectionRequest request;request.identity=TrustedDisplayIdentity::door_bulletin_page;request.door_asset_number=id.value;request.graphics=graphics;request.authenticated_security_level=std::uint16_t{999};auto& c=request.context;
    c.bbs_name=std::string(bbs);c.sysop="Preview Sysop";c.version=std::string(version);
    c.date="2026-09-27";c.time="12:34:56";c.datetime="2026-09-27 12:34:56";
    c.node="1";c.nodes=std::to_string(nodes?nodes:1);c.transport="Preview";c.columns="80";c.rows="25";
    c.name="Preview Caller";c.alias="Preview Caller";c.first_name="Preview";c.city_state="Preview location";c.phone_number="000-000-0000";c.email="preview@example.invalid";
    c.security_level="999";c.logons="1";c.last_call="2026-09-26 12:34:56";c.time_on="60";c.time_left="Unlimited";c.protocol="ZMODEM";c.ip_address="192.0.2.1";c.password_age="1";
    c.original_logon="2026-09-27 12:33:56";c.logon_minutes="1";c.subscription_days="30";c.quiet="Off";c.hotkey="On";
    c.uploads="0";c.downloads="0";c.uploaded_kb="0";c.downloaded_kb="0";c.posts="0";c.hostname="preview.invalid";c.mail_total="0";c.mail_unread="0";c.online="1";c.uptime="60";
    c.message_conference="Preview messages";c.message_conference_number="1";c.message_count="0";c.new_messages="0";c.message_number="1";c.message_to="All";c.message_from="Preview Caller";c.message_subject="Preview subject";c.message_date="2026-09-27 12:34:56";
    c.file_area="Preview files";c.file_area_number="1";c.file_count="0";c.new_files="0";c.filename="SCORE.ASC";c.file_description="Preview scores";c.file_size="0";c.file_uploader="Preview Caller";c.file_date="2026-09-27";c.file_downloads="0";
    c.door_name="Preview Door";c.door_number="1";c.door_menu="Door Menu";c.chat_room="Preview room";c.chat_count="0";c.ftn_network="Preview network";c.ftn_address="1:1/1";
    return request;
}
}
