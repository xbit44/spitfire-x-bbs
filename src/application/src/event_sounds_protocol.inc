std::vector<std::uint8_t> encode_sound_poll_request(const SoundPollRequest& r){
    if(!r.owner.valid() || (!r.epoch.valid() && r.after))return {};
    Writer w;w.raw(r.owner.bytes.data(),16);w.raw(r.epoch.bytes.data(),16);w.u64(r.after);w.u8(r.fresh_desktop_start?1:0);return w.take();
}
std::optional<SoundPollRequest> decode_sound_poll_request(const std::vector<std::uint8_t>& b){
    Reader r(b);SoundPollRequest q;std::uint8_t fresh{};
    if(!r.raw(q.owner.bytes.data(),16)||!r.raw(q.epoch.bytes.data(),16)||!r.u64(q.after)||!r.u8(fresh)||fresh>1||!r.complete()||!q.owner.valid()||(!q.epoch.valid()&&q.after))return {};
    q.fresh_desktop_start=fresh!=0;return q;
}
std::vector<std::uint8_t> encode_sound_poll_snapshot(const SoundPollSnapshot& s){
    if(!s.epoch.valid() || s.events.size()>32 || (!s.owner&&!s.events.empty()))return {};
    Writer w;w.u8(s.owner?1:0);w.raw(s.epoch.bytes.data(),16);w.u64(s.through);
    for(const auto& path:s.configuration.paths){if(!valid_event_sound_path(path)||!w.text(path,kSoundPathMaximumBytes))return {};}
    w.u8(static_cast<std::uint8_t>(s.events.size()));std::uint64_t previous{};
    for(const auto& e:s.events){if(!e.sequence||e.sequence<=previous||e.sequence>s.through||static_cast<std::size_t>(e.event)>=kSoundEventCount)return {};
        w.u64(e.sequence);w.u64(e.at_ms);w.u8(static_cast<std::uint8_t>(e.event));previous=e.sequence;}
    return w.take();
}
std::optional<SoundPollSnapshot> decode_sound_poll_snapshot(const std::vector<std::uint8_t>& b){
    Reader r(b);SoundPollSnapshot s;std::uint8_t owner{},count{};
    if(!r.u8(owner)||owner>1||!r.raw(s.epoch.bytes.data(),16)||!s.epoch.valid()||!r.u64(s.through))return {};
    s.owner=owner!=0;for(auto& path:s.configuration.paths)if(!r.text(path,kSoundPathMaximumBytes)||!valid_event_sound_path(path))return {};
    if(!r.u8(count)||count>32||(!s.owner&&count))return {};
    std::uint64_t previous{};for(unsigned i=0;i<count;++i){SoundEventItem e;std::uint8_t kind{};
        if(!r.u64(e.sequence)||!r.u64(e.at_ms)||!r.u8(kind)||kind>=kSoundEventCount||!e.sequence||e.sequence<=previous||e.sequence>s.through)return {};
        e.event=static_cast<SoundEvent>(kind);previous=e.sequence;s.events.push_back(e);}
    return r.complete()?std::optional<SoundPollSnapshot>(std::move(s)):std::nullopt;
}
