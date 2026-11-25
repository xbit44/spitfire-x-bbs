#pragma once
#include "sfx/application/terminal_stream.hpp"
#include "sfx/application/botgate.hpp"
#include "sfx/platform/network.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <thread>
#include <utility>

namespace sfx::apps::runtime {
namespace net=platform::network;
// Shared caller transport extracted from AlphaTelnetListener. Telnet parsing,
// IAC quoting and partial binary writes remain identical on both platforms.
class AlphaTelnetStream final : public application::IInteractiveSessionByteStream {
public:
    AlphaTelnetStream(const net::Socket socket, const std::atomic<bool>& stopping,
        std::function<void(std::string_view)> output_observer,
        std::function<void(bool)> activity) noexcept
        : socket_(socket), stopping_(stopping), output_observer_(std::move(output_observer)),
          activity_(std::move(activity)) {}

    void enable_menu_polling() noexcept { menu_polling_ = true; }
    bool begin_gateway() noexcept {
        if(binary_mode_||binary_half_iac_||gateway_mode_)return false;
        if(!net::set_nonblocking(socket_,true))return false;
        gateway_mode_=true;return true;
    }
    application::InteractiveStreamWriteResult write_ready(std::string_view bytes){
        if(!gateway_mode_)return {application::InteractiveStreamOutcome::failed,0};
        const auto result=binary_write(bytes);if(result.bytes_written&&output_observer_)output_observer_(bytes.substr(0,result.bytes_written));return result;
    }
    bool finish_gateway_output() noexcept {
        if(!gateway_mode_)return false;
        const auto deadline=net::monotonic_millis()+500;
        while(binary_half_iac_ && net::monotonic_millis()<deadline && !stopping_.load()){
            const char tail=static_cast<char>(kTelnetIac);const auto sent=net::send_bytes(socket_,&tail,1);
            if(sent==1){binary_half_iac_=false;if(output_observer_)output_observer_({&tail,1});break;}
            if(sent!=net::socket_error||!net::retry_receive_error())break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while(!binary_half_iac_ && control_offset_<control_size_ &&
            net::monotonic_millis()<deadline && !stopping_.load()){
            const auto outcome=flush_control();
            if(outcome==application::InteractiveStreamOutcome::completed)break;
            if(outcome!=application::InteractiveStreamOutcome::timed_out)return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return !binary_half_iac_ && !control_failed_ && control_offset_==control_size_;
    }
    bool end_gateway() noexcept {
        if(!gateway_mode_)return false;
        const bool synchronized=finish_gateway_output();
        const bool restored=net::set_nonblocking(socket_,false);gateway_mode_=false;return restored&&synchronized;
    }
    application::InteractiveStreamReadResult read_ready(std::size_t maximum_bytes) {
        using O=application::InteractiveStreamOutcome;
        if(!maximum_bytes || maximum_bytes>application::kInteractiveStreamMaximumReadBytes)return {O::failed,{}};
        if(stopping_.load())return {O::shutdown_requested,{}};
        const auto control=flush_control();
        if(control!=O::completed && control!=O::timed_out)return {control,{}};
        // Only the file pump calls this while it exclusively owns the stream.
        // A socket receive timeout is not a multiplexing primitive: blocking
        // here starves helper output/ACKs on the other side of the same pump.
        const auto selected=net::readable_now(socket_);
        if(selected==net::socket_error)return {O::disconnected,{}};
        if(!selected)return {O::timed_out,{}};
        return read_some(maximum_bytes);
    }
    bool message_binary_mode(bool value) noexcept {
        const std::uint32_t receive=value?100:250,send_timeout=value?100:5000;
        if(!net::set_timeout(socket_,SO_RCVTIMEO,receive)||!net::set_timeout(socket_,SO_SNDTIMEO,send_timeout))return false;
        binary_mode_=value;return !binary_half_iac_;
    }
    bool ansi_terminal() const noexcept { return ansi_terminal_; }

    application::InteractiveStreamReadResult read_some(
        const std::size_t maximum_bytes) override {
        if (maximum_bytes == 0 ||
            maximum_bytes > application::kInteractiveStreamMaximumReadBytes) {
            return {application::InteractiveStreamOutcome::failed, {}};
        }
        try {
            for (;;) {
                const auto control=flush_control();
                if(control!=application::InteractiveStreamOutcome::completed)
                    return {control,{}};
                std::array<std::uint8_t, application::kInteractiveStreamMaximumReadBytes> input{};
                const int received = net::receive_bytes(socket_, reinterpret_cast<char*>(input.data()),
                    static_cast<int>(std::min(maximum_bytes, input.size())));
                if (received == 0) {
                    return {application::InteractiveStreamOutcome::disconnected, {}};
                }
                if (received == net::socket_error) {
                    if (net::retry_receive_error() &&
                        !stopping_.load()) {
                        if (menu_polling_)
                            return {application::InteractiveStreamOutcome::timed_out, {}};
                        continue;
                    }
                    return {stopping_.load()
                            ? application::InteractiveStreamOutcome::shutdown_requested
                            : application::InteractiveStreamOutcome::disconnected,
                        {}};
                }
                std::vector<std::uint8_t> output;
                output.reserve(static_cast<std::size_t>(received));
                for (int index = 0; index < received; ++index) {
                    filter(input[static_cast<std::size_t>(index)], output);
                }
                const auto flushed=flush_control();
                if(flushed!=application::InteractiveStreamOutcome::completed &&
                    flushed!=application::InteractiveStreamOutcome::timed_out)
                    return {flushed,{}};
                if (!output.empty()) {
                    if (activity_) activity_(true); // After IAC filtering, not negotiation.
                    return {application::InteractiveStreamOutcome::completed,
                        std::move(output)};
                }
                if(flushed==application::InteractiveStreamOutcome::timed_out)
                    return {flushed,{}};
                if (menu_polling_)
                    return {application::InteractiveStreamOutcome::timed_out, {}};
            }
        } catch (...) {
            return {application::InteractiveStreamOutcome::failed, {}};
        }
    }

    application::InteractiveStreamWriteResult write_some(
        const std::string_view bytes) override {
        if (bytes.empty()) return {application::InteractiveStreamOutcome::failed, 0};
        if(binary_mode_)return binary_write(bytes);
        try {
            const auto control=flush_control();
            if(control!=application::InteractiveStreamOutcome::completed)return {control,0};
            std::vector<std::uint8_t> encoded;
            encoded.reserve(bytes.size());
            for (const unsigned char byte : bytes) {
                encoded.push_back(byte);
                if (byte == kTelnetIac) encoded.push_back(byte);
            }
            std::size_t offset = 0;
            while (offset < encoded.size()) {
                const std::size_t remaining = encoded.size() - offset;
                const int offered = static_cast<int>(std::min<std::size_t>(
                    remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const int written = net::send_bytes(socket_,
                    reinterpret_cast<const char*>(encoded.data() + offset), offered);
                if (written == net::socket_error || written == 0) {
                    return {stopping_.load()
                            ? application::InteractiveStreamOutcome::shutdown_requested
                            : application::InteractiveStreamOutcome::disconnected,
                        0};
                }
                offset += static_cast<std::size_t>(written);
                if (activity_) activity_(false); // Includes successfully sent partial output.
            }
            if (output_observer_) output_observer_(bytes);
            return {application::InteractiveStreamOutcome::completed, bytes.size()};
        } catch (...) {
            return {application::InteractiveStreamOutcome::failed, 0};
        }
    }

private:
    static constexpr std::uint8_t kTelnetIac=255,kTelnetDo=253,kTelnetDont=254,kTelnetWill=251,kTelnetWont=252,
        kTelnetSubnegotiation=250,kTelnetSubnegotiationEnd=240;
    bool binary_mode_{},binary_half_iac_{},gateway_mode_{};
    // RFC 856 option state is independent of the local transfer/Door lease.
    // Agreeing to BINARY must not seize the stream from ordinary menus.
    bool local_binary_enabled_{},remote_binary_enabled_{};
    std::array<char,8192> control_bytes_{};
    std::size_t control_offset_{},control_size_{};
    bool control_failed_{};
    void queue_control(const std::uint8_t command,const std::uint8_t option) noexcept {
        if(control_size_+3>control_bytes_.size()){control_failed_=true;return;}
        control_bytes_[control_size_++]=static_cast<char>(kTelnetIac);
        control_bytes_[control_size_++]=static_cast<char>(command);
        control_bytes_[control_size_++]=static_cast<char>(option);
    }
    application::InteractiveStreamOutcome flush_control() noexcept {
        using O=application::InteractiveStreamOutcome;
        if(control_failed_)return O::failed;
        // Never insert commands between the two bytes quoting a literal IAC.
        // binary_write (or gateway completion) must finish that byte first.
        if(binary_half_iac_)return O::completed;
        while(control_offset_<control_size_){
            const auto n=net::send_bytes(socket_,control_bytes_.data()+control_offset_,
                static_cast<int>(control_size_-control_offset_));
            if(n==net::socket_error)return net::retry_receive_error()?O::timed_out:O::disconnected;
            if(n<=0)return O::disconnected;
            control_offset_+=static_cast<std::size_t>(n);
        }
        control_offset_=control_size_=0;
        return O::completed;
    }
    application::InteractiveStreamWriteResult binary_write(std::string_view bytes){
        using O=application::InteractiveStreamOutcome;
        if(bytes.empty() || bytes.size()>4096 || (binary_half_iac_ && static_cast<unsigned char>(bytes.front())!=kTelnetIac))return {O::failed,0};
        if(binary_half_iac_ && control_offset_<control_size_){
            const char tail=static_cast<char>(kTelnetIac);
            const auto sent=net::send_bytes(socket_,&tail,1);
            if(sent==net::socket_error)return {net::retry_receive_error()?O::timed_out:O::disconnected,0};
            if(sent!=1)return {O::disconnected,0};
            binary_half_iac_=false;
            if(activity_)activity_(false);
            return {O::completed,1};
        }
        const auto control=flush_control();if(control!=O::completed)return {control,0};
        std::array<char,4096> encoded{};std::size_t offered{},logical{};
        for(const auto c:bytes){const auto count=static_cast<unsigned char>(c)==kTelnetIac && !(logical==0 && binary_half_iac_)?2U:1U;
            if(offered+count>encoded.size())break;
            encoded[offered++]=c;if(count==2)encoded[offered++]=c;++logical;}
        const int sent=net::send_bytes(socket_,encoded.data(),static_cast<int>(offered));
        if(sent==net::socket_error){return {net::retry_receive_error()?O::timed_out:O::disconnected,0};}
        if(sent<=0)return {O::disconnected,0};
        std::size_t complete{},wire{};
        for(;complete<logical;++complete){const auto count=static_cast<unsigned char>(bytes[complete])==kTelnetIac && !(complete==0 && binary_half_iac_)?2U:1U;
            if(wire+count>static_cast<std::size_t>(sent)){binary_half_iac_=true;break;}wire+=count;binary_half_iac_=false;if(wire==static_cast<std::size_t>(sent)){++complete;break;}}
        if(activity_)activity_(false);
        return complete?application::InteractiveStreamWriteResult{O::completed,complete}:application::InteractiveStreamWriteResult{O::timed_out,0};
    }
    bool menu_polling_{};
    enum class InputState : std::uint8_t {
        data,
        iac,
        option,
        subnegotiation,
        subnegotiation_iac
    };

    void filter(const std::uint8_t byte, std::vector<std::uint8_t>& output) noexcept {
        switch (state_) {
            case InputState::data:
                if (byte == kTelnetIac) state_ = InputState::iac;
                else output.push_back(byte);
                break;
            case InputState::iac:
                if (byte == kTelnetIac) {
                    output.push_back(byte);
                    state_ = InputState::data;
                } else if (byte == kTelnetDo || byte == kTelnetDont ||
                    byte == kTelnetWill || byte == kTelnetWont) {
                    option_command_=byte;
                    state_ = InputState::option;
                } else if (byte == kTelnetSubnegotiation) {
                    sub_size_=0;
                    state_ = InputState::subnegotiation;
                } else {
                    state_ = InputState::data;
                }
                break;
            case InputState::option:
                if(byte==0){
                    if(option_command_==kTelnetDo && !local_binary_enabled_){
                        local_binary_enabled_=true;queue_control(kTelnetWill,0);
                    }else if(option_command_==kTelnetDont && local_binary_enabled_){
                        local_binary_enabled_=false;queue_control(kTelnetWont,0);
                    }else if(option_command_==kTelnetWill && !remote_binary_enabled_){
                        remote_binary_enabled_=true;queue_control(kTelnetDo,0);
                    }else if(option_command_==kTelnetWont && remote_binary_enabled_){
                        remote_binary_enabled_=false;queue_control(kTelnetDont,0);
                    }
                }
                if (!binary_mode_ && byte==24 && option_command_==kTelnetWill && !terminal_requested_) {
                    terminal_requested_=true;
                    if(control_size_+6>control_bytes_.size())control_failed_=true;
                    else for(const auto c:std::array<std::uint8_t,6>{255,250,24,1,255,240})
                        control_bytes_[control_size_++]=static_cast<char>(c);
                }
                state_ = InputState::data;
                break;
            case InputState::subnegotiation:
                if (byte == kTelnetIac) state_ = InputState::subnegotiation_iac;
                else if (sub_size_<sub_bytes_.size()) sub_bytes_[sub_size_++]=static_cast<char>(byte);
                break;
            case InputState::subnegotiation_iac:
                if (byte==kTelnetSubnegotiationEnd && terminal_requested_ && sub_size_>=3 &&
                    sub_size_<sub_bytes_.size() && sub_bytes_[0]==24 && sub_bytes_[1]==0)
                    ansi_terminal_=application::botgate_ansi_terminal({sub_bytes_.data()+2,sub_size_-2});
                state_ = byte == kTelnetSubnegotiationEnd
                    ? InputState::data : InputState::subnegotiation;
                break;
        }
    }

    net::Socket socket_{net::invalid_socket};
    const std::atomic<bool>& stopping_;
    std::function<void(std::string_view)> output_observer_;
    std::function<void(bool)> activity_;
    InputState state_{InputState::data};
    std::uint8_t option_command_{};
    std::array<char,67> sub_bytes_{};
    std::size_t sub_size_{};
    bool terminal_requested_{}, ansi_terminal_{};
};

} // namespace sfx::apps::runtime
