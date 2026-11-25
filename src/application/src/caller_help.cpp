#include "sfx/application/caller_help.hpp"
#include <algorithm>
namespace sfx::application {
namespace {
constexpr CallerHelpTopic topics[]{
#include "caller_help_text.inc"
};
const CallerHelpTopic* find(int area,char key) noexcept {
    for(const auto& topic:topics)if(topic.area==area && topic.key==key)return &topic;
    return nullptr;
}
}
const CallerHelpTopic* menu_help_topic(MenuId menu,char key) noexcept {return find(static_cast<int>(menu),key);}
std::vector<MenuTextRow> caller_help_rows(const CallerHelpTopic& topic,unsigned columns){
    const auto width=std::clamp(columns,20U,200U);
    std::vector<MenuTextRow> rows{{MenuTextRole::heading,"["+std::string(topic.key=='/'?"/G":std::string(1,topic.key))+"] "+std::string(topic.title)}};
    auto remaining=topic.text;
    while(!remaining.empty()){
        auto count=std::min<std::size_t>(width,remaining.size());
        if(count<remaining.size()){
            const auto space=remaining.rfind(' ',count);
            if(space!=std::string_view::npos && space)count=space;
        }
        rows.push_back({MenuTextRole::feedback,std::string(remaining.substr(0,count))});
        remaining.remove_prefix(count);
        while(!remaining.empty() && remaining.front()==' ')remaining.remove_prefix(1);
    }
    rows.push_back({MenuTextRole::heading,{}});
    rows.push_back({MenuTextRole::prompt,"< ENTER > To Continue"});return rows;
}
std::string caller_help_question(const MenuSnapshot& snapshot){
    std::string result="Which Command - [";
    for(const auto& command:snapshot.commands){
        if(result.back()!='[')result+=' ';
        result+=command.key=='/'?"/G":std::string(1,command.key);
    }
    return result+"]? ";
}
}
