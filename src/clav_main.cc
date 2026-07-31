#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <exception>
#include <stdexcept>
#include <cstdlib>
#include "clav_window.hpp"
#include "clav_util.hpp"

#pragma comment(lib, "ole32.lib")	// CoInitializeEx / CoCreateInstance
#pragma comment(lib, "shlwapi.lib") // PathUnquoteSpacesW

namespace
{
	// COM (for WIC) needs to be initialized once per thread and torn down
	// after every COM object using it has been released -- RAII guard
	// ensures that ordering regardless of how wWinMain exits.
	struct com_init_guard
	{
		com_init_guard()
		{
			check_hresult(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED),
						  "CoInitializeEx");
		}
		~com_init_guard()
		{
			CoUninitialize();
		}
	};
} // namespace

int WINAPI wWinMain(HINSTANCE hInstance,
					HINSTANCE /*hPrevInstance*/,
					PWSTR pCmdLine,
					int nCmdShow)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	PathUnquoteSpacesW(pCmdLine);

	try
	{
		com_init_guard com;
		clav::win_class wc {L"ClavView_class", hInstance};
		wc.load_icon();
		auto window {wc.create_window(L"ClavView")};

		// Decode + resize/reposition happen before show(): this is pure
		// CPU/WIC work with no GPU device creation, so it's fast, and it
		// means the window appears already at the right size with no
		// visible flash-then-resize.
		window->load_image(pCmdLine);
		window->show(nCmdShow);

		MSG msg {};
		BOOL rv;
		while ((rv = GetMessageW(&msg, nullptr, 0, 0)) != 0)
		{
			if (rv == -1)
			{
				throw std::runtime_error("Couldn't get next message");
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		window->destroy();
		return static_cast<int>(msg.wParam);
	}
	catch (std::exception const& e)
	{
		MessageBoxExW(nullptr,
					  to_wide(e.what()).c_str(),
					  L"Fatal Error",
					  MB_ICONERROR | MB_OK,
					  0);
		return EXIT_FAILURE;
	}
}
