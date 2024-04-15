#include <iostream>
#include <Windows.h>
#include <thread>

#include <dwmapi.h>
#include <d3d11.h>
#include <windowsx.h>

#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_impl_dx11.h"
#include "../external/imgui/imgui_impl_win32.h"

#include "../proc/proc.h"
#include "vector.h"
#include "render.h"

int screenWidth = GetSystemMetrics(SM_CXSCREEN);
int screenHeight = GetSystemMetrics(SM_CYSCREEN);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
	if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) {
		return 0L;
	}

	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0L;
	}
	switch (message)
	{
	case WM_NCHITTEST:
	{
		const LONG borderwidth = GetSystemMetrics(SM_CXSIZEFRAME);
		const LONG titleBarHeight = GetSystemMetrics(SM_CYCAPTION);
		POINT cursorPos = { GET_X_LPARAM(w_param), GET_Y_LPARAM(l_param) };
		RECT windowRect;
		GetWindowRect(window, &windowRect);
		if (cursorPos.y >= windowRect.top && cursorPos.y < windowRect.top + titleBarHeight)
			return HTCAPTION;

		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(window, message, w_param, l_param);
}

namespace offset
{
	constexpr auto dwLocalPlayer = 0x17361E8; // correct
	constexpr auto dwEntityList = 0x18C1DB8; // correct
	constexpr auto dwViewMatrix = 0x19231B0; // correct

	constexpr auto m_iHealth = 0x334; // correct
	constexpr auto dwPlayerPawn = 0x7E4; // maybe correct
	constexpr auto m_iTeamNum = 0x3CB; // correct
	constexpr auto m_vecOrigin = 0x127C; // correct

}

namespace driver {
	namespace codes {
		// Used to setup the driver.
		constexpr ULONG attach =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// Read process memory.
		constexpr ULONG read =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);

		// Write process memory.
		constexpr ULONG write =
			CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
	} // namespace codes

	// Shared between user mode & kernel mode.
	struct Request {
		HANDLE process_id;

		PVOID target;
		PVOID buffer;

		SIZE_T size;
		SIZE_T return_size;
	};
	
	bool attach_to_process(HANDLE driver_handle, const DWORD pid) {
		Request r;
		r.process_id = reinterpret_cast<HANDLE>(pid);

		return DeviceIoControl(driver_handle, codes::attach, &r, sizeof(r), &r, sizeof(r), nullptr,
							   nullptr);
	}

	template <class T>
	T read_memory(HANDLE driver_handle, const std::uintptr_t addr) {
		T temp = {};

		Request r;
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = &temp;
		r.size = sizeof(T);

		DeviceIoControl(driver_handle, codes::read, &r, sizeof(r), &r, sizeof(r), nullptr,
						nullptr);

		return temp;
	}

	template <class T>
	void write_memory(HANDLE driver_handle, const std::uintptr_t addr, const T& value) {
		Request r;
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = (PVOID)&value;
		r.size = sizeof(T);

		DeviceIoControl(driver_handle, codes::write, &r, sizeof(r), &r, sizeof(r), nullptr,
						nullptr);
	}

} // namespace driver

INT APIENTRY WinMain(HINSTANCE instance, HINSTANCE, PSTR, INT cmd_show) {
	const DWORD pid = get_process_id(L"cs2.exe");

	if (pid == 0) {
		std::cout << "Failed to find cs2.\n";
		std::cin.get();
		return 1;
	}

	const HANDLE driver = CreateFile(L"\\\\.\\SDriver", GENERIC_READ, 0, nullptr, OPEN_EXISTING,
									 FILE_ATTRIBUTE_NORMAL, nullptr);
	if (driver == INVALID_HANDLE_VALUE) {
		std::cout << "Failed to create our driver handle.\n";
		std::cin.get();
		return 1;
	}

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = window_procedure;
	wc.hInstance = instance;
	wc.lpszClassName = L"Csgo overlay";

	RegisterClassExW(&wc);

	const HWND overlay = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED,
		wc.lpszClassName,
		L"OVERLAY",
		WS_POPUP,
		0,
		0,
		screenWidth,
		screenHeight,
		nullptr,
		nullptr,
		wc.hInstance,
		nullptr
	);

	SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);
	{
		RECT client_area{};
		GetClientRect(overlay, &client_area);

		RECT window_area{};
		GetWindowRect(overlay, &window_area);

		POINT diff{};
		ClientToScreen(overlay, &diff);

		const MARGINS margins{
			window_area.left + (diff.x - window_area.left),
			window_area.top + (diff.y - window_area.top),
			client_area.right,
			client_area.bottom,
		};
		DwmExtendFrameIntoClientArea(overlay, &margins);
	}

	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferDesc.RefreshRate.Numerator = 60U;
	sd.BufferDesc.RefreshRate.Denominator = 1U;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1U;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2U;
	sd.OutputWindow = overlay;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	constexpr D3D_FEATURE_LEVEL levels[2]{
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_0,
	};

	ID3D11Device* device{ nullptr };
	ID3D11DeviceContext* device_context{ nullptr };
	IDXGISwapChain* swap_chain{ nullptr };
	ID3D11RenderTargetView* render_target_view{ nullptr };
	D3D_FEATURE_LEVEL level{};

	// create device and that
	D3D11CreateDeviceAndSwapChain(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		0U,
		levels,
		2U,
		D3D11_SDK_VERSION,
		&sd,
		&swap_chain,
		&device,
		&level,
		&device_context
	);

	ID3D11Texture2D* back_buffer{ nullptr };
	swap_chain->GetBuffer(0U, IID_PPV_ARGS(&back_buffer));

	if (back_buffer) {
		device->CreateRenderTargetView(back_buffer, nullptr, &render_target_view);
		back_buffer->Release();
	}
	else
		return 1;

	ShowWindow(overlay, cmd_show);
	UpdateWindow(overlay);

	ImGui::CreateContext();
	ImGui::StyleColorsClassic();

	ImGui_ImplWin32_Init(overlay);
	ImGui_ImplDX11_Init(device, device_context);

	if (driver::attach_to_process(driver, pid) == true) {
		std::cout << "Attachment successful.\n";

		if (const std::uintptr_t client = get_module_base(pid, L"client.dll"); client != 0) {
			std::cout << "Client found.\n";

			bool running = true;

			while (running) {
				MSG msg;
				while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);

					if (msg.message == WM_QUIT)
						running = false;
				}

				if (GetAsyncKeyState(VK_END))
					running = false;

				if (!running)
					break;


				uintptr_t localPlayer = driver::read_memory<uintptr_t>(driver, client + offset::dwLocalPlayer);
				Vector3 Localorigin = driver::read_memory<Vector3>(driver, localPlayer + offset::m_vecOrigin);
				view_matrix_t view_matrix = driver::read_memory<view_matrix_t>(driver, client + offset::dwViewMatrix);
				uintptr_t entity_list = driver::read_memory<uintptr_t>(driver, client + offset::dwEntityList);
				int localTeam = driver::read_memory<int>(driver, localPlayer + offset::m_iTeamNum);


				ImGui_ImplDX11_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();


				for (int playerIndex = 1; playerIndex <= 32; ++playerIndex)
				{
					uintptr_t listentry = driver::read_memory<uintptr_t>(driver, entity_list + (8 * (playerIndex & 0x7FFF) >> 9) + 16);

					if (!listentry)
						continue;

					uintptr_t player = driver::read_memory<uintptr_t>(driver, listentry + 120 * (playerIndex & 0x1FF));

					if (!player)
						continue;

					int team = driver::read_memory<int>(driver, player + offset::m_iTeamNum);
					if (localTeam == team)
						continue;


					uint32_t playerPawn = driver::read_memory<uint32_t>(driver, player + offset::dwPlayerPawn);

					uintptr_t listentry2 = driver::read_memory<uintptr_t>(driver, entity_list + 0x8 * ((playerPawn & 0x7FFF) >> 9) + 16);

					if (!listentry2)
						continue;

					uintptr_t pCSPlayerPawn = driver::read_memory<uintptr_t>(driver, listentry2 + 120 * (playerPawn & 0x1FF));

					if (!pCSPlayerPawn)
						continue;

					int health = driver::read_memory<int>(driver, pCSPlayerPawn + offset::m_iHealth);

					if (health <= 0 || health > 100)
						continue;

					if (pCSPlayerPawn == localPlayer)
						continue;


					//correct
					Vector3 origin = driver::read_memory<Vector3>(driver, pCSPlayerPawn + offset::m_vecOrigin);
					Vector3 head = { origin.x, origin.y, origin.z + 75.f };

					Vector3 screenPos = origin.WTS(view_matrix);
					Vector3 screenHead = head.WTS(view_matrix);

					float height = screenPos.y - screenHead.y;
					float width = height / 2.4f;
					//

					RGB enemy = { 255, 0, 0 };

					Render::DrawRect(
						screenHead.x - width / 2,
						screenHead.y,
						width,
						height,
						enemy,
						1.5
					);


				}

				ImGui::Render();
				float color[4]{ 0, 0, 0, 0 };
				device_context->OMSetRenderTargets(1U, &render_target_view, nullptr);
				device_context->ClearRenderTargetView(render_target_view, color);

				ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

				swap_chain->Present(0U, 0U);

				std::this_thread::sleep_for(std::chrono::milliseconds(10));

			}

			// exiting program
			ImGui_ImplDX11_Shutdown();
			ImGui_ImplWin32_Shutdown();

			ImGui::DestroyContext();

			if (swap_chain)
				swap_chain->Release();

			if (device_context)
				device_context->Release();

			if (device)
				device->Release();

			if (render_target_view)
				render_target_view->Release();

			DestroyWindow(overlay);
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
		}
	}

	CloseHandle(driver);

	std::cin.get();

	return 0;
}