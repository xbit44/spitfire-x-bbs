bool valid_ftn_hatch_request(const FtnHatchRequest& r){
    if(!r.operation.valid()||r.file.value<=0||r.file_revision<=0||r.file_revision==INT64_MAX||r.configuration_revision<=0||!valid_ftn_code(r.network)||!valid_ftn_echo_tag(r.tag)||!parse_ftn_address(r.origin)||r.origin.size()>96||r.description.empty()||r.description.size()>4096||sanitize_file_text(r.description,4096,true)!=r.description||r.destinations.empty()||r.destinations.size()>kFtnHatchMaximumDestinations)return false;
    std::set<std::string> selected;for(const auto& link:r.destinations)if(!valid_ftn_code(link)||!selected.insert(key(link)).second)return false;return true;
}
std::vector<std::uint8_t> encode_ftn_hatch_request(const FtnHatchRequest& r){
    if(!valid_ftn_hatch_request(r))return {};
    std::vector<std::uint8_t> out;out.reserve(60000);
    const auto number=[&](std::uint64_t n,unsigned count){for(unsigned i=0;i<count;++i)out.push_back(static_cast<std::uint8_t>(n>>(8*i)));};
    const auto text=[&](const std::string& value){number(value.size(),2);out.insert(out.end(),value.begin(),value.end());};
    number(1,1);out.insert(out.end(),r.operation.bytes.begin(),r.operation.bytes.end());number(static_cast<std::uint64_t>(r.file.value),8);number(static_cast<std::uint64_t>(r.file_revision),8);number(static_cast<std::uint64_t>(r.configuration_revision),8);
    text(r.network);text(r.tag);text(r.origin);text(r.description);number(r.destinations.size(),2);for(const auto& link:r.destinations)text(link);
    return out.size()<=60000?out:std::vector<std::uint8_t>{};
}
std::optional<FtnHatchRequest> decode_ftn_hatch_request(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>60000)return {};
    Reader input{bytes};if(input.n(1)!=1||bytes.size()-input.at<16)return {};FtnHatchRequest r;
    std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(input.at),16,r.operation.bytes.begin());input.at+=16;
    const auto file=input.n(8),revision=input.n(8),configuration=input.n(8);if(file>INT64_MAX||revision>INT64_MAX||configuration>INT64_MAX)return {};
    r.file.value=static_cast<std::int64_t>(file);r.file_revision=static_cast<std::int64_t>(revision);r.configuration_revision=static_cast<std::int64_t>(configuration);
    r.network=input.s(48);r.tag=input.s(128);r.origin=input.s(96);r.description=input.s(4096);const auto count=input.n(2);if(count>kFtnHatchMaximumDestinations)return {};
    for(std::uint64_t i=0;i<count&&input.good;++i)r.destinations.push_back(input.s(48));
    return input.good&&input.at==bytes.size()&&valid_ftn_hatch_request(r)?std::optional<FtnHatchRequest>{std::move(r)}:std::nullopt;
}
