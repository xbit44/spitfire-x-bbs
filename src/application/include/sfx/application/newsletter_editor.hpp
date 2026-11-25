#pragma once
#include "sfx/application/message_editor.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <functional>

namespace sfx::application {
// SFXEditor's whole-document mode. Bytes are authoritative; tabs and line
// separators are never passed through message-body normalization.
class NewsletterDocument final {
public:
    static constexpr std::size_t maximum_bytes=256U*1024U;
    bool load(std::string);
    const std::string& bytes()const noexcept{return bytes_;}
    bool apply(MessageEditorEvent);
    bool replace_line(std::size_t,std::string_view);
    bool insert_line(std::size_t,std::string_view);
    bool delete_line(std::size_t);
    std::vector<std::string> lines()const;
    std::string frame(unsigned columns,unsigned rows);
    std::string update_frame(unsigned columns,unsigned rows,MessageEditorScreen&);
private:
    struct Line {std::size_t begin,end,next;};
    std::vector<Line> index()const;
    bool splice(std::size_t,std::size_t,std::string_view);
    std::size_t row()const;
    std::size_t visual_column(std::size_t,std::size_t)const;
    std::string bytes_,newline_{"\r\n"};
    std::size_t cursor_{},top_{},left_{};
    unsigned page_rows_{18};
    bool inserting_{true};
};
struct NewsletterSnapshot {bool available{},exists{};std::string bytes,identity,notice;};
struct NewsletterSaveResult {bool saved{};std::string notice;};
struct NewsletterEditorPorts {
    ICallerTerminal& terminal;
    std::function<bool()> authorized,ansi;
    std::function<unsigned()> columns,rows;
    std::function<bool(std::string_view)> write;
    std::function<StreamCallerTerminal::EditorByteResult()> byte;
    std::function<void(bool)> accepted_key;
    std::function<NewsletterSnapshot()> load;
    std::function<NewsletterSaveResult(const NewsletterSnapshot&,std::string_view)> save;
};
bool edit_sysop_newsletter(NewsletterEditorPorts);
}
