#include <Windows.h>
#include <iostream>
#include <vector>

#include <gl/GL.h>
#pragma comment(lib, "OpenGL32.lib")

#include "SDL/SDL.h"
#pragma comment (lib, "SDL/SDL.lib")

#include "minhook/minhook.h"
#pragma comment (lib, "minhook/minhook.lib")

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_opengl2.h"
#include "imgui/imgui_impl_win32.h"

using wglSwapBuffers_t = BOOL(WINAPI*)(HDC);
wglSwapBuffers_t pwglSwapBuffers = NULL;

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
SetCursorPos_t pSetCursorPos = NULL;

WNDPROC pWndProc;

HWND gameHWND;

HMODULE base_addr;

DWORD recoil_instruction_address1;
DWORD recoil_instruction_address2;
DWORD recoil_instruction_address3;
DWORD recoil_instruction_address4;

DWORD spread_instruction_address;

LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace vars
{
	bool menu_open = false;
	namespace visuals
	{
		bool enable;
		bool teammates;
		bool box;
		bool name;
		bool health;
		float red_team_col[3];
		float blue_team_col[3];
	}
	namespace misc
	{
		bool no_recoil;
		bool no_spread;
	}
	void load_default_settings()
	{
		visuals::enable = true;
		visuals::teammates = true;
		visuals::box = true;
		visuals::name = true;
		visuals::health = true;

		visuals::red_team_col[0] = 1.f;
		visuals::blue_team_col[2] = 1.f;
	}
}

namespace fonts
{
	ImFont* font_Main;
	ImFont* font_Credits;
}

class Matrix4x4
{
public:
	union
	{
		struct
		{
			float        _11, _12, _13, _14;
			float        _21, _22, _23, _24;
			float        _31, _32, _33, _34;
			float        _41, _42, _43, _44;
		};
		float m[4][4];
	};
};

namespace console
{
    FILE* out;
    void attach()
    {
        if (AllocConsole())
        {
            freopen_s(&out, "conout$", "w", stdout);
        }
    }
	void free()
	{
		fclose(out);
		FreeConsole();
	}
}

namespace memory_utils
{
	#ifdef _WIN64
		#define DWORD_OF_BITNESS DWORD64
		#define PTRMAXVAL ((PVOID)0x000F000000000000)
	#elif _WIN32
		#define DWORD_OF_BITNESS DWORD
		#define PTRMAXVAL ((PVOID)0xFFF00000)
	#endif

	bool is_valid_ptr(PVOID ptr)
	{
		return (ptr >= (PVOID)0x10000) && (ptr < PTRMAXVAL) && ptr != nullptr && !IsBadReadPtr(ptr, sizeof(ptr));
	}

	DWORD_OF_BITNESS base;
	DWORD_OF_BITNESS get_base()
	{
		if (!base)
		{
			base = (DWORD_OF_BITNESS)GetModuleHandle(0);
			return base;
		}
		else
		{
			return base;
		}
	}

	template<class T>
	void write(std::vector<DWORD_OF_BITNESS>address, T value, bool this_string)
	{
		size_t lengh_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < lengh_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return;

			if (i < lengh_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				T* writable_address = (T*)(relative_address + address[lengh_array]);
				*writable_address = value;
			}
		}
	}

	template<class T>
	T read(std::vector<DWORD_OF_BITNESS>address)
	{
		size_t lengh_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < lengh_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return *static_cast<T*>(0);

			if (i < lengh_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				T readable_address = *(T*)(relative_address + address[lengh_array]);
				return readable_address;
			}
		}
	}

	char* read_string(std::vector<DWORD_OF_BITNESS>address)
	{
		size_t lengh_array = address.size() - 1;
		DWORD_OF_BITNESS relative_address;
		relative_address = address[0];
		for (int i = 1; i < lengh_array + 1; i++)
		{
			if (is_valid_ptr((LPVOID)relative_address) == false)
				return NULL;

			if (i < lengh_array)
				relative_address = *(DWORD_OF_BITNESS*)(relative_address + address[i]);
			else
			{
				char* readable_address = (char*)(relative_address + address[lengh_array]);
				return readable_address;
			}
		}
	}

	DWORD_OF_BITNESS get_module_size(DWORD_OF_BITNESS address)
	{
		return PIMAGE_NT_HEADERS(address + (DWORD_OF_BITNESS)PIMAGE_DOS_HEADER(address)->e_lfanew)->OptionalHeader.SizeOfImage;
	}

	DWORD_OF_BITNESS find_pattern(HMODULE module, const char* pattern, const char* mask)
	{
		DWORD_OF_BITNESS base = (DWORD_OF_BITNESS)module;
		DWORD_OF_BITNESS size = get_module_size(base);

		DWORD_OF_BITNESS patternLength = (DWORD_OF_BITNESS)strlen(mask);

		for (DWORD_OF_BITNESS i = 0; i < size - patternLength; i++)
		{
			bool found = true;
			for (DWORD_OF_BITNESS j = 0; j < patternLength; j++)
			{
				found &= mask[j] == '?' || pattern[j] == *(char*)(base + i + j);
			}

			if (found)
			{
				return base + i;
			}
		}

		return NULL;
	}

	void patch_instruction(DWORD_OF_BITNESS instruction_address, const char* instruction_bytes, int sizeof_instruction_byte)
	{
		DWORD dwOldProtection;

		VirtualProtect((LPVOID)instruction_address, sizeof_instruction_byte, PAGE_EXECUTE_READWRITE, &dwOldProtection);

		memcpy((LPVOID)instruction_address, instruction_bytes, sizeof_instruction_byte);

		VirtualProtect((LPVOID)instruction_address, sizeof_instruction_byte, dwOldProtection, NULL);

		FlushInstructionCache(GetCurrentProcess(), (LPVOID)instruction_address, sizeof_instruction_byte);
	}
}

namespace game_utils
{
	Matrix4x4 get_matrix()
	{
		return memory_utils::read<Matrix4x4>({ memory_utils::get_base(), 0x101AE8 });
	}

	bool WorldToScreen(const float vIn[3], float* flOut)
	{
		Matrix4x4 view_projection = get_matrix();

		float w = view_projection.m[0][3] * vIn[0] + view_projection.m[1][3] * vIn[1] + view_projection.m[2][3] * vIn[2] + view_projection.m[3][3];

		if (w < 0.01)
			return false;

		flOut[0] = view_projection.m[0][0] * vIn[0] + view_projection.m[1][0] * vIn[1] + view_projection.m[2][0] * vIn[2] + view_projection.m[3][0];
		flOut[1] = view_projection.m[0][1] * vIn[0] + view_projection.m[1][1] * vIn[1] + view_projection.m[2][1] * vIn[2] + view_projection.m[3][1];

		float invw = 1.0f / w;

		flOut[0] *= invw;
		flOut[1] *= invw;

		int width, height;

		auto io = ImGui::GetIO();
		width = io.DisplaySize.x;
		height = io.DisplaySize.y;

		float x = (float)width / 2;
		float y = (float)height / 2;

		x += 0.5 * flOut[0] * (float)width + 0.5;
		y -= 0.5 * flOut[1] * (float)height + 0.5;

		flOut[0] = x;
		flOut[1] = y;

		return true;
	}

	int max_players_on_map()
	{
		return memory_utils::read<int>({ memory_utils::get_base(), 0x10F500 });
	}

	void patch_recoil(bool enable)
	{
		if (enable)
		{
			memory_utils::patch_instruction(recoil_instruction_address1, "\x90\x90\x90", 3);
			memory_utils::patch_instruction(recoil_instruction_address2, "\x90\x90\x90", 3);
			memory_utils::patch_instruction(recoil_instruction_address3, "\x90\x90\x90", 3);
			memory_utils::patch_instruction(recoil_instruction_address4, "\x90\x90\x90", 3);
		}
		else
		{
			memory_utils::patch_instruction(recoil_instruction_address1, "\xD9\x53\x4C", 3);
			memory_utils::patch_instruction(recoil_instruction_address2, "\xD9\x5B\x4C", 3);
			memory_utils::patch_instruction(recoil_instruction_address3, "\xD9\x5A\x4C", 3);
			memory_utils::patch_instruction(recoil_instruction_address4, "\xD9\x5A\x4C", 3);
		}
	}

	void patch_spread(bool enable)
	{
		if (enable)
		{
			memory_utils::patch_instruction(spread_instruction_address, "\x90\x90\x90", 3);
		}
		else
		{
			memory_utils::patch_instruction(spread_instruction_address, "\xFF\x46\x1C", 3);
		}
	}

	enum {
		RED_TEAM,
		BLUE_TEAM,
		SPECTATOR = 4
	};
}

namespace drawing
{
	void AddCircle(const ImVec2& position, float radius, const ImColor& color, int segments)
	{
		auto window = ImGui::GetCurrentWindow();

		window->DrawList->AddCircle(position, radius, ImGui::ColorConvertFloat4ToU32(color), segments);
	}

	void AddRect(const ImVec2& position, const ImVec2& size, const ImColor& color, float rounding = 0.f)
	{
		auto window = ImGui::GetCurrentWindow();

		window->DrawList->AddRect(position, size, ImGui::ColorConvertFloat4ToU32(color), rounding);
	}

	void AddRectFilled(const ImVec2& position, const ImVec2& size, const ImColor& color, float rounding)
	{
		auto window = ImGui::GetCurrentWindow();

		window->DrawList->AddRectFilled(position, size, ImGui::ColorConvertFloat4ToU32(color), rounding);
	}

	void DrawBox(float x, float y, float w, float h, const ImColor& color)
	{
		AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color);
	}

	void DrawFillArea(float x, float y, float w, float h, const ImColor& color, float rounding = 0.f)
	{
		AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), color, rounding);
	}

	void DrawEspBox(float x, float y, float w, float h, float r, float g, float b, float a)
	{
		if (vars::visuals::box == false)
			return;

		DrawBox(x, y, w, h, ImColor(r, g, b, a));
	}

	enum
	{
		FL_NONE = 1 << 0,
		FL_SHADOW = 1 << 1,
		FL_OUTLINE = 1 << 2,
		FL_CENTER_X = 1 << 3,
		FL_CENTER_Y = 1 << 4
	};

	void AddText(float x, float y, const ImColor& color, int flags, const char* format, ...)
	{
		int style = 0;

		if (!format)
			return;

		auto& io = ImGui::GetIO();
		auto DrawList = ImGui::GetWindowDrawList();
		auto Font = io.Fonts->Fonts[0];

		char szBuff[256] = { '\0' };
		va_list vlist = nullptr;
		va_start(vlist, format);
		vsprintf_s(szBuff, format, vlist);
		va_end(vlist);

		DrawList->PushTextureID(io.Fonts->TexID);

		float size = Font->FontSize;
		ImVec2 text_size = Font->CalcTextSizeA(size, FLT_MAX, 0.f, szBuff);

		ImColor Color = ImColor(0.f, 0.f, 0.f, color.Value.w);

		if (flags & FL_CENTER_X)
			x -= text_size.x / 2.f;

		if (flags & FL_CENTER_Y)
			y -= text_size.x / 2.f;

		if (style == 1)
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);

		if (style == 2)
		{
			DrawList->AddText(Font, size, ImVec2(x, y - 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x - 1.f, y), ImGui::ColorConvertFloat4ToU32(Color), szBuff);

			DrawList->AddText(Font, size, ImVec2(x - 1.f, y - 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y - 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x - 1.f, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
			DrawList->AddText(Font, size, ImVec2(x + 1.f, y + 1.f), ImGui::ColorConvertFloat4ToU32(Color), szBuff);
		}

		DrawList->AddText(Font, size, ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(color), szBuff);
		DrawList->PopTextureID();
	}

	void DrawName(const char* pcszPlayerName, float x, float y, float w, ImColor col)
	{
		if (vars::visuals::name == false)
			return;

		if (pcszPlayerName == NULL)
			return;

		ImFont* Font = ImGui::GetIO().Fonts->Fonts[0];
		ImVec2 text_size = Font->CalcTextSizeA(Font->FontSize, FLT_MAX, 0, "");

		AddText(x + w / 2.f, y - text_size.y - 2.f, ImColor(1.f, 1.f, 1.f, col.Value.w), FL_CENTER_X, u8"%s", pcszPlayerName);
	}

	void DrawHealth(float x, float y, float h, float health, float max_health, ImColor col)
	{
		if (vars::visuals::health == false)
			return;

		health = ImClamp(health, 0.f, max_health);

		const auto size = h / max_health * health;
		const auto thickness = 2.f;

		DrawFillArea(x - thickness - 1.9f, y + h, thickness, -size, ImColor(0.f, 1.f, 0.f, col.Value.w));

		DrawBox(x - thickness - 2.9f, y - 1.f, thickness + 2.f, h + 2.f, ImColor(0.f, 0.f, 0.f, col.Value.w));
	}
}

namespace scene
{
	void run_visuals()
	{
		if (vars::visuals::enable == false)
			return;

		DWORD local_player = memory_utils::read<DWORD>( { memory_utils::get_base(), 0x10F4F4 } );

		if (local_player == NULL)
			return;

		int my_team = memory_utils::read<int>( { local_player, 0x32C } );

		DWORD entity_list = memory_utils::read<DWORD>({ memory_utils::get_base(), 0x10F4F8 });

		for (int i = 0; i < game_utils::max_players_on_map(); i++)
		{
			DWORD entity = memory_utils::read<DWORD>({ entity_list, (DWORD)(i * 0x4) });

			if (entity == NULL)
				continue;

			ImColor col;

			int team = memory_utils::read<int>({ entity, 0x32C });

			if (team == game_utils::SPECTATOR)
				continue;

			if (vars::visuals::teammates && team == my_team)
				continue;

			if (team == game_utils::BLUE_TEAM)
				col = ImColor(vars::visuals::blue_team_col[0], vars::visuals::blue_team_col[1], vars::visuals::blue_team_col[2]);
			else if (team == game_utils::RED_TEAM)
				col = ImColor(vars::visuals::red_team_col[0], vars::visuals::red_team_col[1], vars::visuals::red_team_col[2]);

			int health = memory_utils::read<int>({ entity, 0xF8 });

			if (health <= NULL)
				continue;

			char* name = memory_utils::read_string({ entity, 0x225 });

			float origin_bot[3];
			origin_bot[0] = memory_utils::read<float>({ entity, 0x34 });
			origin_bot[1] = memory_utils::read<float>({ entity, 0x38 });
			origin_bot[2] = memory_utils::read<float>({ entity, 0x3C });

			float origin_top[3] = { origin_bot[0], origin_bot[1], origin_bot[2] };
			origin_top[2] += 5.f;

			float out_bot[2], out_top[2];
			if (game_utils::WorldToScreen(origin_bot, out_bot) && game_utils::WorldToScreen(origin_top, out_top))
			{
				float h = out_bot[1] - out_top[1];
				float w = h / 2;
				float x = out_bot[0] - w / 2;
				float y = out_top[1];

				drawing::DrawEspBox(x, y, w, h, col.Value.x, col.Value.y, col.Value.z, 1.f);

				drawing::DrawName(name, x, y, w, ImColor(1.f, 1.f, 1.f));

				drawing::DrawHealth(x, y, h, health, 100.f, col);
			}
		}
	}

	void begin()
	{
		static auto once = []() -> int
		{
			ImGui::CreateContext();
			ImGui::StyleColorsClassic();

			auto& style = ImGui::GetStyle();

			style.FrameRounding = 3.f;
			style.ChildRounding = 3.f;
			style.ChildBorderSize = 1.f;
			style.ScrollbarSize = 0.6f;
			style.ScrollbarRounding = 3.f;
			style.GrabRounding = 3.f;
			style.WindowRounding = 0.f;

			style.Colors[ImGuiCol_FrameBg] = ImColor(200, 200, 200);
			style.Colors[ImGuiCol_FrameBgHovered] = ImColor(220, 220, 220);
			style.Colors[ImGuiCol_FrameBgActive] = ImColor(230, 230, 230);
			style.Colors[ImGuiCol_Separator] = ImColor(180, 180, 180);
			style.Colors[ImGuiCol_CheckMark] = ImColor(255, 172, 19);
			style.Colors[ImGuiCol_SliderGrab] = ImColor(255, 172, 19);
			style.Colors[ImGuiCol_SliderGrabActive] = ImColor(255, 172, 19);
			style.Colors[ImGuiCol_ScrollbarBg] = ImColor(120, 120, 120);
			style.Colors[ImGuiCol_ScrollbarGrab] = ImColor(255, 172, 19);
			style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImGui::GetStyleColorVec4(ImGuiCol_ScrollbarGrab);
			style.Colors[ImGuiCol_ScrollbarGrabActive] = ImGui::GetStyleColorVec4(ImGuiCol_ScrollbarGrab);
			style.Colors[ImGuiCol_Header] = ImColor(160, 160, 160);
			style.Colors[ImGuiCol_HeaderHovered] = ImColor(200, 200, 200);
			style.Colors[ImGuiCol_Button] = ImColor(180, 180, 180);
			style.Colors[ImGuiCol_ButtonHovered] = ImColor(200, 200, 200);
			style.Colors[ImGuiCol_ButtonActive] = ImColor(230, 230, 230);
			style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.78f, 0.78f, 0.78f, 1.f);
			style.Colors[ImGuiCol_WindowBg] = ImColor(220, 220, 220, 0.7 * 255);
			style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.40f, 0.40f, 0.80f, 0.20f);
			style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.40f, 0.40f, 0.55f, 0.80f);
			style.Colors[ImGuiCol_Border] = ImVec4(0.72f, 0.72f, 0.72f, 0.70f);
			style.Colors[ImGuiCol_TitleBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.83f);
			style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.75f, 0.75f, 0.75f, 0.87f);
			style.Colors[ImGuiCol_Text] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
			style.Colors[ImGuiCol_ChildBg] = ImVec4(0.72f, 0.72f, 0.72f, 0.76f);
			style.Colors[ImGuiCol_PopupBg] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
			style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.81f, 0.81f, 0.81f, 1.00f);
			style.Colors[ImGuiCol_Tab] = ImVec4(0.61f, 0.61f, 0.61f, 0.79f);
			style.Colors[ImGuiCol_TabHovered] = ImVec4(0.71f, 0.71f, 0.71f, 0.80f);
			style.Colors[ImGuiCol_TabActive] = ImVec4(0.77f, 0.77f, 0.77f, 0.84f);
			style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.73f, 0.73f, 0.73f, 0.82f);
			style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.58f, 0.58f, 0.58f, 0.84f);

			auto& io = ImGui::GetIO();
			io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Verdana.ttf", 15.0f, NULL, io.Fonts->GetGlyphRangesCyrillic());
			fonts::font_Main = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Verdana.ttf", 21.f);
			fonts::font_Credits = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Verdana.ttf", 15.f);
			ImGui_ImplWin32_Init(gameHWND);
			ImGui_ImplOpenGL2_Init();

			ImGuiWindowFlags flags_color_edit = ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoInputs;
			ImGui::SetColorEditOptions(flags_color_edit);

			std::cout << __FUNCTION__ << " first called!" << std::endl;

			return true;
		}();

		ImGui_ImplOpenGL2_NewFrame();
		ImGui_ImplWin32_NewFrame();

		ImGui::NewFrame();

		if (game_utils::max_players_on_map() == 0)
		{
			if (vars::misc::no_recoil)
			{
				game_utils::patch_recoil(false);
				vars::misc::no_recoil = false;
			}
		}

		if (vars::menu_open)
		{
			ImGui::Begin("main window", nullptr);
			ImGui::BeginChild("main menu", ImVec2(), true);
			ImGui::Text("Visuals");
			ImGui::Checkbox("Enable", &vars::visuals::enable);
			ImGui::Checkbox("Teammates", &vars::visuals::teammates);
			ImGui::Checkbox("Box", &vars::visuals::box);
			ImGui::Checkbox("Name", &vars::visuals::name);
			ImGui::Checkbox("Health", &vars::visuals::health);
			ImGui::ColorEdit3("Red team color", vars::visuals::red_team_col);
			ImGui::SameLine();
			ImGui::ColorEdit3("Blue team color", vars::visuals::blue_team_col);
			ImGui::Text("Misc");
			std::string isEnable_NoRecoil = vars::misc::no_recoil ? "Disable no recoil" : "Enable no recoil";
			if (game_utils::max_players_on_map() > 0)
			{
				if (ImGui::Button(isEnable_NoRecoil.c_str()))
				{
					vars::misc::no_recoil = !vars::misc::no_recoil;
					game_utils::patch_recoil(vars::misc::no_recoil);
				}
			}
			std::string isEnable_NoSpread = vars::misc::no_spread ? "Disable no spread" : "Enable no spread";
			if (ImGui::Button(isEnable_NoSpread.c_str()))
			{
				vars::misc::no_spread = !vars::misc::no_spread;
				game_utils::patch_spread(vars::misc::no_spread);
			}

			ImGui::EndChild();
			ImGui::End();
		}

		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4());
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
		ImGui::Begin("##BackBuffer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
		ImGui::SetWindowPos(ImVec2(), ImGuiCond_Always);
		ImGui::SetWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y), ImGuiCond_Always);

		run_visuals();

		ImGui::GetCurrentWindow()->DrawList->PushClipRectFullScreen();
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();

		ImGui::EndFrame();

		ImGui::Render();

		ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
	}
}

BOOL WINAPI wglSwapbuffers_Hooked(HDC hdc)
{
	scene::begin();

	return pwglSwapBuffers(hdc);
}

HRESULT WINAPI WndProc_Hooked(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	static auto once = []()
	{
		std::cout << __FUNCTION__ << " first called!" << std::endl;

		return true;
	}();

	if (Msg == WM_KEYDOWN && wParam == VK_INSERT)
		vars::menu_open = !vars::menu_open;

	if (vars::menu_open)
	{
		ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
	}

	return CallWindowProc(pWndProc, hWnd, Msg, wParam, lParam);
}

int EventFilter(void* userdata, SDL_Event* event)
{
	if (vars::menu_open)
	{
		SDL_ShowCursor(true);
	}
	else
	{
		SDL_ShowCursor(false);
	}

	return vars::menu_open ? 0 : 1;
}

void setup_hack(LPVOID module) 
{
    console::attach();

	gameHWND = FindWindow(NULL, "AssaultCube");

	if (gameHWND == NULL)
	{
		std::cout << __FUNCTION__ << " not found window\n";
		FreeLibraryAndExitThread((HMODULE)module, 1);
	}

	pWndProc = (WNDPROC)SetWindowLong(gameHWND, GWL_WNDPROC, (LONG)&WndProc_Hooked);

	MH_Initialize();

	HMODULE opengl32 = GetModuleHandle("opengl32.dll");

	LPVOID wglswapbuffers_address = (LPVOID)GetProcAddress(opengl32, "wglSwapBuffers");

	MH_CreateHook(wglswapbuffers_address, &wglSwapbuffers_Hooked, (LPVOID*)&pwglSwapBuffers);
	MH_EnableHook(wglswapbuffers_address);

	SDL_SetEventFilter(EventFilter, 0);

	base_addr = GetModuleHandle(NULL);

	recoil_instruction_address1 = memory_utils::find_pattern(base_addr, "\xD9\x53\x4C\xD9\x05\x7C\xE6\x4E\x00", "xxxxxxxxx");
	recoil_instruction_address2 = memory_utils::find_pattern(base_addr, "\xD9\x5B\x4C\xEB\x04", "xxxxx");
	recoil_instruction_address3 = memory_utils::find_pattern(base_addr, "\xD9\x5A\x4C\x5F\x5E", "xxxxx");
	recoil_instruction_address4 = memory_utils::find_pattern(base_addr, "\xD9\x5A\x4C\x5E\x5B", "xxxxx");

	spread_instruction_address = memory_utils::find_pattern(base_addr, "\xFF\x46\x1C\x38\x9A\x28\x01\x00\x00", "?xxxxxxxx");

	vars::load_default_settings();

    while (true)
    {
        if (GetAsyncKeyState(VK_DELETE))
            break;
		
		Sleep(10);
    }

	if (vars::misc::no_recoil)
	{
		game_utils::patch_recoil(false);
	}

	if (vars::misc::no_spread)
	{
		game_utils::patch_spread(false);
	}

	SetWindowLong(gameHWND, GWL_WNDPROC, (LONG)pWndProc);
	Sleep(100);

	MH_DisableHook(wglswapbuffers_address);
	MH_RemoveHook(wglswapbuffers_address);
	Sleep(100);

	MH_Uninitialize();
	Sleep(100);

	SDL_SetEventFilter(0, 0);
	Sleep(100);

	std::cout << "free library\n";

    FreeLibraryAndExitThread((HMODULE)module, 0);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(0, 0, (LPTHREAD_START_ROUTINE)setup_hack, hModule, 0, 0);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
