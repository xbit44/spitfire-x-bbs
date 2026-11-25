#pragma once

#include "sfx/application/terminal_stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfx::test {

struct ByteStreamReadObservation {
    std::size_t requested_maximum{0};
    application::InteractiveStreamOutcome outcome{
        application::InteractiveStreamOutcome::failed};
    std::size_t returned_bytes{0};
};

struct ByteStreamWriteObservation {
    std::size_t offered_bytes{0};
    application::InteractiveStreamOutcome outcome{
        application::InteractiveStreamOutcome::failed};
    std::size_t accepted_bytes{0};
};

class DeterministicInteractiveByteStreamFake final
    : public application::IInteractiveSessionByteStream {
public:
    ~DeterministicInteractiveByteStreamFake() override {
        for (auto& script : reads_) {
            erase(script.bytes);
        }
    }

    void script_non_secret_bytes(std::vector<std::uint8_t> bytes) {
        reads_.push_back(ReadScript::raw(std::move(bytes)));
    }

    void script_non_secret_text(const std::string_view text) {
        script_non_secret_bytes(
            std::vector<std::uint8_t>(text.begin(), text.end()));
    }

    void script_generated_secret_input(
        const std::size_t secret_length,
        const std::size_t maximum_fragment_bytes =
            std::numeric_limits<std::size_t>::max(),
        const bool append_enter = true) {
        reads_.push_back(ReadScript::generated_secret(
            secret_length, maximum_fragment_bytes, append_enter));
    }

    void script_read_outcome(
        const application::InteractiveStreamOutcome outcome) {
        reads_.push_back(ReadScript::stop(outcome));
    }

    void script_malformed_read(
        const application::InteractiveStreamOutcome outcome,
        std::vector<std::uint8_t> bytes = {}) {
        reads_.push_back(
            ReadScript::malformed(outcome, std::move(bytes)));
    }

    void script_read_exception() {
        reads_.push_back(ReadScript::throw_exception());
    }

    void script_write(
        const application::InteractiveStreamOutcome outcome,
        const std::size_t maximum_accepted_bytes) {
        writes_.push_back(
            {outcome, maximum_accepted_bytes, false});
    }

    void script_write_exception(
        const std::size_t maximum_accepted_bytes = 0) {
        writes_.push_back(
            {application::InteractiveStreamOutcome::failed,
                maximum_accepted_bytes, true});
    }

    application::InteractiveStreamReadResult read_some(
        const std::size_t maximum_bytes) override {
        if (reads_.empty() || maximum_bytes == 0 ||
            maximum_bytes >
                application::kInteractiveStreamMaximumReadBytes) {
            read_observations_.push_back({maximum_bytes,
                application::InteractiveStreamOutcome::failed, 0});
            return {application::InteractiveStreamOutcome::failed, {}};
        }

        ReadScript& script = reads_.front();
        if (script.kind == ReadKind::throw_exception) {
            reads_.pop_front();
            read_observations_.push_back(
                {maximum_bytes,
                    application::InteractiveStreamOutcome::failed, 0});
            throw std::runtime_error(
                "scripted byte-stream read exception");
        }
        if (script.kind == ReadKind::malformed) {
            const auto outcome = script.outcome;
            auto returned = std::move(script.bytes);
            reads_.pop_front();
            read_observations_.push_back(
                {maximum_bytes, outcome, returned.size()});
            return {outcome, std::move(returned)};
        }
        if (script.kind == ReadKind::stop) {
            const auto outcome = script.outcome;
            reads_.pop_front();
            read_observations_.push_back({maximum_bytes, outcome, 0});
            return {outcome, {}};
        }

        std::vector<std::uint8_t> returned;
        if (script.kind == ReadKind::raw) {
            const std::size_t available =
                script.bytes.size() - script.offset;
            const std::size_t count = std::min(maximum_bytes, available);
            returned.insert(returned.end(),
                script.bytes.begin() +
                    static_cast<std::ptrdiff_t>(script.offset),
                script.bytes.begin() +
                    static_cast<std::ptrdiff_t>(script.offset + count));
            application::detail::explicitly_erase_bytes(
                script.bytes.data() + script.offset, count);
            script.offset += count;
            if (script.offset == script.bytes.size()) {
                erase(script.bytes);
                reads_.pop_front();
            }
        } else {
            const std::size_t fragment =
                std::min(maximum_bytes, script.maximum_fragment_bytes);
            const std::size_t generated =
                std::min(fragment, script.generated_remaining);
            returned.assign(generated, static_cast<std::uint8_t>('X'));
            script.generated_remaining -= generated;
            if (returned.size() < fragment &&
                script.generated_remaining == 0 &&
                script.enter_pending) {
                returned.push_back(static_cast<std::uint8_t>('\r'));
                script.enter_pending = false;
            }
            if (script.generated_remaining == 0 &&
                !script.enter_pending) {
                reads_.pop_front();
            }
        }

        read_observations_.push_back({maximum_bytes,
            application::InteractiveStreamOutcome::completed,
            returned.size()});
        return {application::InteractiveStreamOutcome::completed,
            std::move(returned)};
    }

    application::InteractiveStreamWriteResult write_some(
        const std::string_view bytes) override {
        application::InteractiveStreamOutcome outcome =
            application::InteractiveStreamOutcome::completed;
        std::size_t maximum = bytes.size();
        bool throw_after_accept = false;
        if (!writes_.empty()) {
            outcome = writes_.front().outcome;
            maximum = writes_.front().maximum_accepted_bytes;
            throw_after_accept =
                writes_.front().throw_after_accept;
            writes_.pop_front();
        }
        const std::size_t accepted = std::min(maximum, bytes.size());
        output_.append(bytes.data(), accepted);
        write_observations_.push_back(
            {bytes.size(), outcome, accepted});
        if (throw_after_accept) {
            throw std::runtime_error(
                "scripted byte-stream write exception");
        }
        return {outcome, accepted};
    }

    const std::string& output() const noexcept {
        return output_;
    }

    const std::vector<ByteStreamReadObservation>&
    read_observations() const noexcept {
        return read_observations_;
    }

    const std::vector<ByteStreamWriteObservation>&
    write_observations() const noexcept {
        return write_observations_;
    }

    std::size_t pending_reads() const noexcept {
        return reads_.size();
    }

    std::size_t pending_writes() const noexcept {
        return writes_.size();
    }

    std::size_t retained_secret_bytes() const noexcept {
        return 0;
    }

private:
    enum class ReadKind : std::uint8_t {
        raw,
        generated_secret,
        stop,
        malformed,
        throw_exception
    };

    struct ReadScript {
        ReadKind kind{ReadKind::stop};
        application::InteractiveStreamOutcome outcome{
            application::InteractiveStreamOutcome::failed};
        std::vector<std::uint8_t> bytes;
        std::size_t offset{0};
        std::size_t generated_remaining{0};
        std::size_t maximum_fragment_bytes{
            std::numeric_limits<std::size_t>::max()};
        bool enter_pending{false};

        static ReadScript raw(std::vector<std::uint8_t> bytes) {
            ReadScript result;
            result.kind = ReadKind::raw;
            result.outcome =
                application::InteractiveStreamOutcome::completed;
            result.bytes = std::move(bytes);
            return result;
        }

        static ReadScript generated_secret(
            const std::size_t length,
            const std::size_t maximum_fragment,
            const bool append_enter) {
            ReadScript result;
            result.kind = ReadKind::generated_secret;
            result.outcome =
                application::InteractiveStreamOutcome::completed;
            result.generated_remaining = length;
            result.maximum_fragment_bytes =
                std::max<std::size_t>(1U, maximum_fragment);
            result.enter_pending = append_enter;
            return result;
        }

        static ReadScript stop(
            const application::InteractiveStreamOutcome outcome) {
            ReadScript result;
            result.kind = ReadKind::stop;
            result.outcome = outcome;
            return result;
        }

        static ReadScript malformed(
            const application::InteractiveStreamOutcome outcome,
            std::vector<std::uint8_t> bytes) {
            ReadScript result;
            result.kind = ReadKind::malformed;
            result.outcome = outcome;
            result.bytes = std::move(bytes);
            return result;
        }

        static ReadScript throw_exception() {
            ReadScript result;
            result.kind = ReadKind::throw_exception;
            result.outcome =
                application::InteractiveStreamOutcome::failed;
            return result;
        }
    };

    struct WriteScript {
        application::InteractiveStreamOutcome outcome;
        std::size_t maximum_accepted_bytes;
        bool throw_after_accept;
    };

    static void erase(std::vector<std::uint8_t>& bytes) noexcept {
        if (!bytes.empty()) {
            application::detail::explicitly_erase_bytes(
                bytes.data(), bytes.size());
            bytes.clear();
        }
    }

    std::deque<ReadScript> reads_;
    std::deque<WriteScript> writes_;
    std::string output_;
    std::vector<ByteStreamReadObservation> read_observations_;
    std::vector<ByteStreamWriteObservation> write_observations_;
};

}  // namespace sfx::test
