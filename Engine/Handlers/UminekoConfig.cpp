/**
 *  Shared Config additions for Umineko Project language scripts.
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Handlers/UminekoConfig.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace {

// The existing menu supplies its translated labels, buttons and layout.
// Only the engine's additions live here, independent of en/wh/ru.file.
// Variable 8075 is the Discord setting used by earlier English release scripts.
constexpr std::string_view ConfigScript = R"ONS(
*onsc_end
end

*onsc_l
lsp 106,":s;#FFFFFF#FF0000`{p:11:Restart Game}",0,0
lsp 107,set_ctrl,0,0
lsp 316,":s;#FFFFFF`{p:8:Discord Rich Presence}",135+3840,835
align_buttons_l 104,106
return

*onsc_r
align_buttons_r 103,102,101,107
return

*onsc_s
gosub *settings_op_ed_song_subtitles
gosub *onsc_d
return

*onsc_d
operate_config u_read,$8075,"discord_presence"
mov %8075,1
if $8075 == "false" mov %8075,0
if $8075 == "0" mov %8075,0
if $8075 == "off" mov %8075,0
if $8075 == "no" mov %8075,0
notif %8075 = 1 lsp 273,set_on,settings_base_off1+%mcoord_add,835
if %8075 = 1 lsp 273,set_on2,settings_base_off1+%mcoord_add,835
notif %8075 = 0 lsp 274,set_off,settings_base_off2+%mcoord_add,835
if %8075 = 0 lsp 274,set_off2,settings_base_off2+%mcoord_add,835
return

*onsc_b
spbtn 106,375
spbtn 107,376
spbtn 273,151
spbtn 274,152
btnwait %BtnRes
if %BtnRes = 375 relaunch
if %BtnRes = 376 gosub *onsc_controls : mov %BtnRes,0
if %BtnRes = 47 inc %msgwnd_type : mod %msgwnd_type,4 : gosub *settings_textbox : mov %BtnRes,0
if %BtnRes = 48 add %msgwnd_type,3 : mod %msgwnd_type,4 : gosub *settings_textbox : mov %BtnRes,0
if %BtnRes = 151 discord_presence 1 : operate_config u_write,"true","discord_presence" : operate_config u_save : gosub *onsc_d : mov %BtnRes,0
if %BtnRes = 152 discord_presence 0 : operate_config u_write,"false","discord_presence" : operate_config u_save : gosub *onsc_d : mov %BtnRes,0
return

*onsc_t
lsp 247,":a/2,0,3;graphics\menu_en\config\config_left.png",1145+%mcoord_add,493
lsp 248,":a/2,0,3;graphics\menu_en\config\config_right.png",1665+%mcoord_add,493
_csp 249,250
_csp2 262
_csp 263
if %msgwnd_type = 3 lsp 263,":s;#FFFFFF`{p:8:{w:360:}{a:c:No Window}}",1250+%mcoord_add,493 : return
mov $8075,localisation
if localisation == "wh" mov $8075,"en"
if %CHIRU_MODE = 1 mov $8075,"ep5_"+$8075
mov $8075,"graphics\system\wnd\msgwnd_preview_"+$8075+".png"
if %msgwnd_type = 1 mov $8075,"graphics\system\wnd\msgwnd_preview2.png"
if %msgwnd_type = 2 mov $8075,"graphics\system\wnd\msgwnd_preview3.png"
fileexist %8075,$8075
if %8075 = 1 lsp2 262,":a;"+$8075,1430+%mcoord_add,520,32,32,0 : return
; Older installations may not include preview artwork. Keep the selector usable.
if %msgwnd_type = 0 mov $8075,"TypeL"
if %msgwnd_type = 1 mov $8075,"TypeB"
if %msgwnd_type = 2 mov $8075,"TypeN"
lsp 263,":s;#FFFFFF`{p:8:{w:360:}{a:c:"+$8075+"}}",1250+%mcoord_add,493
return

*onsc_controls
lsp 5,":c;graphics\colour\black.png",0,0,180
if %interface_kb_dual = 0 lsp 3,":s;#FFFFFF`{p:11:{w:1700:{a:c:{c:FFAA00:Left click / Enter / Space} - Advance text or activate the selected button.{n}{c:FFAA00:Right click / Esc / Z} - Open or close the pause menu; Esc can also hide text.{n}{c:FFAA00:Mouse wheel up / Left arrow / H} - Open the backlog from text.{n}{c:FFAA00:Mouse wheel down / Right arrow / L} - Advance or hide text when enabled.{n}{c:FFAA00:A} - Automode. {c:FFAA00:Ctrl} - fast-skip while held. {c:FFAA00:Alt+S / R1} - toggle skip.{n}{c:FFAA00:F} - Toggle fullscreen. {c:FFAA00:Alt+M} - Mute. {c:FFAA00:Alt+E} - Screenshot.{n}{n}Click, press Enter/Space, right-click, or press Esc to close.}}}",0,0
if %interface_kb_dual = 1 lsp 3,":s;#FFFFFF`{p:11:{w:1700:{a:c:{c:FFAA00:Cross / A} - Advance text or activate the selected button.{n}{c:FFAA00:Circle / B} - Cancel or go back.{n}{c:FFAA00:Square / X or Options / Start} - Open or close the pause menu.{n}{c:FFAA00:D-pad / sticks} - Navigate buttons and scroll choices when available.{n}{c:FFAA00:Triangle / Y} - Open the Message Browser from text.{n}{c:FFAA00:L1} - Automode. {c:FFAA00:R1} - Toggle skip.{n}{c:FFAA00:Share / Back} - Mute.{n}{n}Press Cross/A or Circle/B to close.}}}",0,0
align_message_c 3
mov %msg_x,110
amsp 3,%msg_x,%msg_y
lsp 2,set_ctrl,0,0
getspsize 2,%msg_tmp_w,%msg_tmp_h
mov %msg_x,1920-%msg_tmp_w
div %msg_x,2
sub %msg_y,%msg_tmp_h+35
amsp 2,%msg_x,%msg_y
print 23
btndef ""
*onsc_controls_wait
btnwait2 %BtnRes
if %BtnRes = 0 _csp 2,5 : return
if %BtnRes = -1 _csp 2,5 : return
if %BtnRes = -10 _csp 2,5 : return
if %BtnRes = -11 _csp 2,5 : return
goto *onsc_controls_wait
)ONS";

std::string_view trim(std::string_view text) {
	const auto start = text.find_first_not_of(" \t");
	if (start == std::string_view::npos)
		return {};
	return text.substr(start, text.find_last_not_of(" \t") - start + 1);
}

} // namespace

uint32_t extendUminekoConfig(std::vector<uint8_t> &script, size_t length) {
	const std::string_view source(reinterpret_cast<const char *>(script.data()), length);
	if (!source.substr(0, 256).contains(";gameid UminekoPS3fication") || source.contains("\n*onsc_"))
		return 0;

	struct Edit {
		size_t offset;
		size_t length;
		std::string_view command;
	};
	std::vector<Edit> edits;

	auto section = [&](const char *first, const char *last) -> std::string_view {
		const auto begin = source.find(std::string("\n*") + first + '\n');
		if (begin == std::string_view::npos)
			return {};
		const auto end = source.find(std::string("\n*") + last + '\n', begin + 1);
		if (end == std::string_view::npos)
			return {};
		return source.substr(begin + 1, end - begin);
	};

	// Hooks must fit in the existing lines. Never move the original script's
	// labels, lines or byte offsets: saved call stacks and read history use them.
	auto hook = [&](std::string_view block, std::string_view command, std::string_view replacement, bool required = true) {
		size_t matches = 0;
		while (!block.empty()) {
			const auto end = block.find('\n');
			const auto line = block.substr(0, end);
			if (trim(line.substr(0, line.find(';'))) == command) {
				if (replacement.size() > line.size())
					return false;
				edits.push_back({static_cast<size_t>(line.data() - source.data()), line.size(), replacement});
				++matches;
			}
			if (end == std::string_view::npos)
				break;
			block.remove_prefix(end + 1);
		}
		return required ? matches == 1 : matches <= 1;
	};

	const auto elements = section("settings_elems", "settings_loop");
	const auto buttons = section("settings_loop", "settings_move");
	const auto textbox = section("settings_textbox", "settings_textspeed");
	if (elements.empty() || buttons.empty() || textbox.empty())
		return 0;

	const bool oldFooter = elements.contains("align_buttons_l 104,106");
	if (!hook(elements, oldFooter ? "align_buttons_l 104,106" : "align_buttons_l 104", "gosub *onsc_l") ||
	    !hook(elements, oldFooter ? "align_buttons_r 103,102,101,107" : "align_buttons_r 103,102,101", "gosub *onsc_r") ||
	    !hook(elements, "gosub *settings_op_ed_song_subtitles", "gosub *onsc_s") ||
	    !hook(buttons, "btnwait %BtnRes", "gosub *onsc_b"))
		return 0;

	// The original and previously enhanced English menus have different textbox
	// code. Redirect at their shared first executable line, leaving both intact.
	const auto firstTextboxLine = textbox.find("\n\tlsp 247,");
	const auto oldTextboxLine = textbox.find("\n\tnotif %msgwnd_type = 2 lsp 247,");
	const auto textboxLine = firstTextboxLine != std::string_view::npos ? firstTextboxLine : oldTextboxLine;
	if (textboxLine == std::string_view::npos)
		return 0;
	const auto textboxEnd = textbox.find('\n', textboxLine + 1);
	if (textboxEnd == std::string_view::npos)
		return 0;
	edits.push_back({static_cast<size_t>(textbox.data() - source.data()) + textboxLine + 1,
	                 textboxEnd - textboxLine - 1, "goto *onsc_t"});

	// Supersede additions in older bundled English scripts, without duplicating
	// buttons or requiring users to replace any of their language files.
	for (const auto command : {"lsp 106,set_restart,0,0", "lsp 107,set_ctrl,0,0",
	                           "lsp 316,set_discord_presence,135+3840,835", "gosub *settings_discord_presence"}) {
		if (!hook(elements, command, "", false))
			return 0;
	}
	for (const auto command : {"spbtn 106,375", "spbtn 107,376", "spbtn 273,151", "spbtn 274,152", "spbtn 249,49", "spbtn 250,50"}) {
		if (!hook(buttons, command, "", false))
			return 0;
	}

	// Validate every hook before changing anything. Unsupported script revisions
	// retain their original menu rather than receiving a partially installed UI.
	for (const auto &edit : edits) {
		std::fill_n(script.begin() + edit.offset, edit.length, ' ');
		std::copy(edit.command.begin(), edit.command.end(), script.begin() + edit.offset);
	}
	script.resize(length);
	script.insert(script.end(), ConfigScript.begin(), ConfigScript.end());
	script.push_back('\0');
	uint32_t addedLabels = 0;
	for (size_t pos = 0; (pos = ConfigScript.find("\n*", pos)) != std::string_view::npos; pos += 2)
		++addedLabels;
	return addedLabels;
}
