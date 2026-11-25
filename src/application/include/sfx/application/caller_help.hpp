#pragma once
#include "sfx/application/menu.hpp"
namespace sfx::application {
struct CallerHelpTopic { int area; char key; std::string_view title,text; };
const CallerHelpTopic* menu_help_topic(MenuId,char) noexcept;
std::vector<MenuTextRow> caller_help_rows(const CallerHelpTopic&,unsigned columns=80);
std::string caller_help_question(const MenuSnapshot&);
}
