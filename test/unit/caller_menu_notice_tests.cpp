#include "sfx/application/caller_menu_notice.hpp"
#include "sfx/application/offline_notice.hpp"
#include "sfx/application/caller_inactivity.hpp"
#include "sfx/application/login_inactivity_stream.hpp"
#include <iostream>
#include <stdexcept>

class Reader final : public sfx::application::ITrustedDisplayAssetReader {
public:
    std::string source{"Custom offline\r\n"};
    std::string selected;
    std::size_t limit{};
    sfx::application::TrustedDisplayAssetReadResult read_asset(
        std::string_view name, std::size_t maximum) override {
        selected = name;
        limit = maximum;
        return {sfx::application::TrustedDisplayAssetReadCode::available,
            std::string(name), "fixture-file", source};
    }
};

class PollingLogin final : public sfx::application::IInteractiveSessionByteStream {
public:
    std::uint64_t now{}, key_at{};
    bool fail_output{};
    std::string output;
    sfx::application::InteractiveStreamReadResult read_some(std::size_t) override {
        now += 1000;
        if (key_at && now == key_at)
            return {sfx::application::InteractiveStreamOutcome::completed, {'x'}};
        return {sfx::application::InteractiveStreamOutcome::timed_out, {}};
    }
    sfx::application::InteractiveStreamWriteResult write_some(std::string_view bytes) override {
        if (fail_output) return {sfx::application::InteractiveStreamOutcome::failed, 0};
        const auto count = std::min<std::size_t>(3, bytes.size()); // exercise partial writes
        output.append(bytes.substr(0, count));
        return {sfx::application::InteractiveStreamOutcome::completed, count};
    }
};

int main() {
    unsigned checks = 0;
    const auto require = [&](bool value) {
        ++checks;
        if (!value) throw std::runtime_error("caller menu notice assertion");
    };
    try {
        using Notice = sfx::application::CallerMenuNotice;
        Notice notice;
        require(notice.update(0, 1000) == Notice::Change::none);
        require(notice.update(301000, 1000) == Notice::Change::show);
        require(notice.text(1000) == "\r\nSpitfire-X BBS will shut down in 5 minutes.\r\nPlease save your work and log off soon.\r\n<enter>\r\n");
        for (const auto duration : {1000U, 59000U, 60000U, 300000U, 900000U}) {
            Notice wrapped;
            require(wrapped.update(1000 + duration, 1000) == Notice::Change::show);
            const auto text = wrapped.text(1000);
            std::size_t start = 0;
            while (start < text.size()) {
                const auto end = text.find("\r\n", start);
                require(end != std::string::npos && end - start < 80);
                start = end + 2;
            }
            require(text.find(".\r\nPlease save your work and log off soon.\r\n<enter>\r\n") != std::string::npos);
        }
        require(notice.update(301000, 2000) == Notice::Change::none);
        require(!notice.acknowledge('G') && notice.waiting());
        require(notice.acknowledge('\r') && !notice.waiting());
        require(!notice.acknowledge('\n'));
        require(notice.update(301000, 3000) == Notice::Change::none);
        require(notice.update(0, 3000) == Notice::Change::none);
        require(notice.update(64000, 4000) == Notice::Change::show);
        require(notice.text(4000).find("1 minute.") != std::string::npos);
        require(notice.update(0, 5000) == Notice::Change::redraw && !notice.waiting());
        require(notice.text(5000).empty());
        require(notice.update(9000, 5000) == Notice::Change::show);
        require(notice.text(5000).find("4 seconds.") != std::string::npos);
        require(notice.text(8001).find("1 second.") != std::string::npos);
        require(notice.text(9000).empty());
        require(notice.update(10000, 10000) == Notice::Change::redraw);
        namespace app = sfx::application;
        const auto fallback = app::offline_notice("Test HQ", nullptr);
        require(fallback == "SF-X (Test HQ) is currently offline, please call back soon.\r\n");
        Reader reader;
        require(app::offline_notice("Test HQ", &reader) == "Custom offline\r\n");
        require(reader.selected == "NONODES.ASC");
        require(reader.limit == 4096);
        for (const auto literal : {"sysop@example.test", "@@ @BBSNAME@ @PASSWORD@", "first\r\nlast", "tab\ttext"}) {
            reader.source = literal;
            require(app::offline_notice("Test HQ", &reader) == literal);
        }
        reader.source = "\x1b[31mCustom offline\x1b[0m\r\n";
        require(app::offline_notice("Test HQ", &reader) == fallback);
        require(app::offline_notice("Test HQ", &reader, app::GraphicsMode::ansi) != fallback);
        require(reader.selected == "NONODES.ANS");
        reader.source = "\x1b]0;unsafe\a";
        require(app::offline_notice("Test HQ", &reader, app::GraphicsMode::ansi) == fallback);
        reader.source = std::string(4097, 'x');
        require(app::offline_notice("Test HQ", &reader) == fallback);
        reader.source = " \r\n";
        require(app::offline_notice("Test HQ", &reader) == fallback);
        reader.source = std::string(4096, 'x');
        require(app::offline_notice("Test HQ", &reader) == reader.source);
        reader.source = "text\a";
        require(app::offline_notice("Test HQ", &reader) == fallback);
        reader.source = std::string("text\0tail", 9);
        require(app::offline_notice("Test HQ", &reader) == fallback);
        reader.source = std::string("\xff\xfe", 2);
        require(app::offline_notice("Test HQ", &reader) == reader.source);
        using Idle = app::CallerInactivity;
        Idle idle(1000, 5, false);
        require(idle.poll(240999) == Idle::Action::none);
        require(idle.poll(241000) == Idle::Action::warn);
        require(idle.poll(241001) == Idle::Action::none);
        require(idle.poll(300999) == Idle::Action::none);
        require(idle.poll(301000) == Idle::Action::disconnect);
        idle.keyboard(301000);
        require(idle.poll(540999) == Idle::Action::none);
        require(idle.poll(541000) == Idle::Action::warn);
        require(idle.poll(601000) == Idle::Action::disconnect);
        Idle short_idle(0, 2, false);
        require(short_idle.poll(60000) == Idle::Action::warn);
        require(short_idle.poll(120000) == Idle::Action::disconnect);
        Idle sysop(0, 5, true);
        require(sysop.poll(86400000) == Idle::Action::none);
        require(app::kCallerInactivityWarning == "\r\nYou have another minute to execute\r\na command or you will be logged off.\r\n");
        PollingLogin login;
        const std::string warning="\r\nLogin idle: disconnecting in 10 seconds.\r\n";
        app::LoginInactivityStream guarded(login, 60, [&] { return login.now; });
        require(guarded.read_some(1).outcome == app::InteractiveStreamOutcome::timed_out);
        require(login.now == 60000 && login.output == warning);
        PollingLogin typed; typed.key_at = 30000;
        app::LoginInactivityStream reset(typed, 60, [&] { return typed.now; });
        const auto key = reset.read_some(1);
        require(key.outcome == app::InteractiveStreamOutcome::completed && key.bytes == std::vector<std::uint8_t>{'x'});
        require(typed.output.empty());
        require(reset.read_some(1).outcome == app::InteractiveStreamOutcome::timed_out);
        require(typed.now == 90000 && typed.output == warning);
        PollingLogin broken; broken.fail_output = true;
        app::LoginInactivityStream failure(broken, 60, [&] { return broken.now; });
        require(failure.read_some(1).outcome == app::InteractiveStreamOutcome::failed);
        require(broken.now == 50000);
        PollingLogin ending;
        unsigned endings=0;
        app::LoginInactivityStream notice_on_end(ending,60,[&]{return ending.now;},[&]{
            ++endings;ending.output+="Optional SFASLEEP";
        });
        require(notice_on_end.read_some(1).outcome==app::InteractiveStreamOutcome::timed_out);
        require(ending.now==60000 && endings==1 && ending.output==warning+"Optional SFASLEEP");
        require(notice_on_end.read_some(1).outcome==app::InteractiveStreamOutcome::timed_out && endings==1);
        std::cout << "PASS caller menu notice checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " check=" << checks << '\n';
        return 1;
    }
}
