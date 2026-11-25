#include "sfx/application/message_receive.hpp"
namespace sfx::application {
bool same_message_receive_context(const ComposeContext& a,const ComposeContext& b) noexcept {
    return a.target.kind==b.target.kind && a.target.echo==b.target.echo && a.target.recipient==b.target.recipient &&
        a.target.sysop_assignment_revision==b.target.sysop_assignment_revision && a.target.remote==b.target.remote && a.parent==b.parent &&
        a.group_revision==b.group_revision && a.echo_revision==b.echo_revision && a.recipient_revision==b.recipient_revision &&
        a.limits_revision==b.limits_revision && a.maximum_lines==b.maximum_lines &&
        a.presentation_revision==b.presentation_revision && a.pipe_colors==b.pipe_colors && a.ftn_configuration_revision==b.ftn_configuration_revision;
}
bool valid_message_receive_channel(const DecodedMessageReceiveChannel& c) noexcept {
    return c.acquire && c.release && c.read && c.write && c.now && c.absolute_deadline && c.authorized;
}
} // namespace sfx::application
