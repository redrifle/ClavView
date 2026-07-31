#include "clav_window.hpp"
#include "clav_util.hpp"
#include <stdexcept>
#include <dwmapi.h>
#include "resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

#define IDC_ARROW_W MAKEINTRESOURCEW(32512)
#define IDI_APPLICATION_W MAKEINTRESOURCEW(32512)

// Posted from the background decode thread to the UI thread. Carries
// ownership of the decoded WIC bitmap across the thread boundary --
// the UI thread is responsible for deleting it (see WM_IMAGE_DECODED).
constexpr UINT WM_IMAGE_DECODED {WM_APP + 1};

struct decode_result
{
	com_ptr<IWICFormatConverter> converter;
	unsigned long long generation;
	bool is_thumbnail;
};

void apply_modern_frame_style(HWND hwnd)
{
	/*
	Windows 11 (build 22000+): DWMWA_SYSTEMBACKDROP_TYPE, a
	documented, stable public API (Mica/Acrylic).

	Windows 10: no public equivalent. SetWindowCompositionAttribute
	is an *undocumented* user32 export that ships the Acrylic blur
	used internally by Windows 10's own shell; it has been stable
	across Win10 releases in practice, but Microsoft gives no
	compatibility guarantee for it, so failures here are treated as
	"feature unavailable," never as errors.

	Manually-defined constants below aren't guaranteed to be present in
	whatever Windows SDK version this is built against, so they're
	spelled out numerically rather than relying on headers.
	*/

	constexpr int DWMWA_USE_IMMERSIVE_DARK_MODE_ {20};
	constexpr int DWMWA_BORDER_COLOR_ {34};
	constexpr int DWMWA_CAPTION_COLOR_ {35};
	constexpr int DWMWA_SYSTEMBACKDROP_TYPE_ {38};
	constexpr DWORD DWMSBT_MAINWINDOW_ {2}; // Mica

	enum ACCENT_STATE_
	{
		ACCENT_ENABLE_ACRYLICBLURBEHIND_ = 4
	};

	struct ACCENT_POLICY_
	{
		DWORD AccentState;
		DWORD AccentFlags;
		DWORD GradientColor; // 0xAABBGGRR
		DWORD AnimationId;
	};

	struct WINDOWCOMPOSITIONATTRIBDATA_
	{
		DWORD Attrib; // WCA_ACCENT_POLICY == 19
		PVOID pvData;
		SIZE_T cbData;
	};

	using SetWindowCompositionAttributeFn = BOOL(
		WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA_*);

	COLORREF const accent {RGB(0x21, 0x21, 0x21)};
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR_, &accent, sizeof(accent));
	DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR_, &accent, sizeof(accent));

	BOOL const dark_mode {TRUE};
	DwmSetWindowAttribute(hwnd,
						  DWMWA_USE_IMMERSIVE_DARK_MODE_,
						  &dark_mode,
						  sizeof(dark_mode));

	HRESULT const mica_hr {DwmSetWindowAttribute(hwnd,
												 DWMWA_SYSTEMBACKDROP_TYPE_,
												 &DWMSBT_MAINWINDOW_,
												 sizeof(DWMSBT_MAINWINDOW_))};

	if (SUCCEEDED(mica_hr))
	{
		return;
	}

	HMODULE const user32 {GetModuleHandleW(L"user32.dll")};
	if (!user32)
	{
		return;
	}

	auto const set_composition_attribute {
		reinterpret_cast<SetWindowCompositionAttributeFn>(
			GetProcAddress(user32, "SetWindowCompositionAttribute"))};
	if (!set_composition_attribute)
	{
		return;
	}

	ACCENT_POLICY_ policy {ACCENT_ENABLE_ACRYLICBLURBEHIND_,
						   2,
						   0x99202020, // translucent dark tint, 0xAABBGGRR
						   0};

	WINDOWCOMPOSITIONATTRIBDATA_ data {19, // WCA_ACCENT_POLICY
									   &policy,
									   sizeof(policy)};

	set_composition_attribute(hwnd, &data);
}

LRESULT CALLBACK clav::window::s_WindowProc(HWND hwnd,
											UINT uMsg,
											WPARAM wParam,
											LPARAM lParam)
{
	clav::window* pw;

	if (uMsg == WM_NCCREATE)
	{
		LPCREATESTRUCTW pcs {reinterpret_cast<LPCREATESTRUCTW>(lParam)};
		pw = reinterpret_cast<clav::window*>(pcs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pw));
		pw->handle_ = hwnd;
		++pw->owner_.live_windows_;
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}

	LONG_PTR const lp {GetWindowLongPtrW(hwnd, GWLP_USERDATA)};
	pw = reinterpret_cast<clav::window*>(lp);

	if (pw)
	{
		return pw->WindowProc(hwnd, uMsg, wParam, lParam);
	}
	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT clav::window::WindowProc(HWND hwnd,
								 UINT uMsg,
								 WPARAM wParam,
								 LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE || wParam == 0x51)
		{
			DestroyWindow(hwnd);
		}
		break;

	case WM_ENTERSIZEMOVE:
		in_sizemove_ = true;
		break;

	case WM_EXITSIZEMOVE:
		in_sizemove_ = false;
		break;

	case WM_ERASEBKGND:
		/* D2D Clear() repaints the whole client area every frame; skipping
		GDI's erase avoids a flicker source during resize. */
		return 1;

	case WM_SIZE:
		if (render_target_)
		{
			UINT const w {LOWORD(lParam)}, h {HIWORD(lParam)};
			render_target_->Resize(D2D1::SizeU(w, h));
		}
		break;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		render();
		EndPaint(hwnd, &ps);
		break;
	}

	case WM_IMAGE_DECODED: {
		// unique_ptr guarantees the heap allocation from spawn_decode() is
		// freed on every path, including the stale-generation early-out.
		std::unique_ptr<decode_result> result {
			reinterpret_cast<decode_result*>(lParam)};

		if (result->generation != generation_.load())
		{
			return 0; // superseded by a newer load_image() call
		}

		ensure_device_resources();

		com_ptr<ID2D1Bitmap> new_bitmap;
		HRESULT const hr {
			render_target_->CreateBitmapFromWicBitmap(result->converter.get(),
													  nullptr,
													  new_bitmap.put())};

		if (SUCCEEDED(hr))
		{
			bitmap_ = std::move(new_bitmap);
			last_converter_cache_ = std::move(result->converter);
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		break;
	}

	case WM_NCDESTROY:
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		handle_ = nullptr;
		break;

	case WM_DESTROY:
		--owner_.live_windows_;
		if (owner_.live_windows_ == 0)
		{
			PostQuitMessage(0);
		}
		break;

	default:
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}
	return 0;
}

void clav::window::ensure_device_resources()
{
	if (!d2d_factory_)
	{
		check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
										d2d_factory_.put()),
					  "D2D1CreateFactory");
	}

	if (!render_target_)
	{
		RECT rc;
		GetClientRect(handle_, &rc);
		D2D1_SIZE_U const size {
			D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left),
						static_cast<UINT32>(rc.bottom - rc.top))};

		check_hresult(d2d_factory_->CreateHwndRenderTarget(
						  D2D1::RenderTargetProperties(),
						  D2D1::HwndRenderTargetProperties(
							  handle_,
							  size,
							  D2D1_PRESENT_OPTIONS_IMMEDIATELY),
						  render_target_.put()),
					  "CreateHwndRenderTarget");
	}

	// Rebuild the GPU bitmap from the cached CPU-side WIC converter -- this
	// only fires after device loss (D2DERR_RECREATE_TARGET); the normal
	// path is the async WM_IMAGE_DECODED handler in WindowProc.
	if (!bitmap_ && last_converter_cache_)
	{
		check_hresult(render_target_->CreateBitmapFromWicBitmap(
						  last_converter_cache_.get(),
						  nullptr,
						  bitmap_.put()),
					  "CreateBitmapFromWicBitmap");
	}
}

void clav::window::discard_device_resources()
{
	bitmap_.reset();
	render_target_.reset();
}

void clav::window::render()
{
	if (!handle_)
	{
		return;
	}

	ensure_device_resources();
	render_target_->BeginDraw();
	render_target_->Clear(D2D1::ColorF(33.f / 255, 33.f / 255, 33.f / 255));

	if (bitmap_)
	{
		D2D1_SIZE_F const rt_size {render_target_->GetSize()};
		double const cw {rt_size.width}, ch {rt_size.height};
		double const img_aspect {static_cast<double>(img_width_) /
								 img_height_};
		double const win_aspect {cw / ch};

		double dest_w, dest_h;
		if (img_aspect > win_aspect)
		{
			dest_w = cw;
			dest_h = cw / img_aspect;
		}
		else
		{
			dest_h = ch;
			dest_w = ch * img_aspect;
		}

		double const dest_x {(cw - dest_w) / 2.0};
		double const dest_y {(ch - dest_h) / 2.0};

		D2D1_RECT_F const dest_rect {
			D2D1::RectF(static_cast<float>(dest_x),
						static_cast<float>(dest_y),
						static_cast<float>(dest_x + dest_w),
						static_cast<float>(dest_y + dest_h))};

		// LINEAR is the highest quality ID2D1HwndRenderTarget supports and
		// is fully GPU-accelerated -- smooth at any window size, at any
		// zoom level, with no per-frame CPU resampling cost.
		render_target_->DrawBitmap(bitmap_.get(),
								   dest_rect,
								   1.0f,
								   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
	}

	HRESULT const hr {render_target_->EndDraw()};
	if (hr == D2DERR_RECREATE_TARGET)
	{
		discard_device_resources();
		InvalidateRect(handle_, nullptr, FALSE);
	}
}

RECT clav::window::calculate_window_rect(unsigned int client_w,
										 unsigned int client_h) const
{
	DWORD const style {
		static_cast<DWORD>(GetWindowLongPtrW(handle_, GWL_STYLE))};
	DWORD const ex_style {
		static_cast<DWORD>(GetWindowLongPtrW(handle_, GWL_EXSTYLE))};
	BOOL const has_menu {GetMenu(handle_) != nullptr};
	UINT const dpi {GetDpiForWindow(handle_)};
	RECT rect {0, 0, static_cast<LONG>(client_w), static_cast<LONG>(client_h)};

	check_hresult(
		AdjustWindowRectExForDpi(&rect, style, has_menu, ex_style, dpi)
			? S_OK
			: HRESULT_FROM_WIN32(GetLastError()),
		"AdjustWindowRectExForDpi");

	return rect;
}

void clav::window::position_for_image(unsigned int native_w,
									  unsigned int native_h)
{
	wants_maximized_ = false; // reset in case a smaller image is loaded later

	RECT const wr {calculate_window_rect(native_w, native_h)};
	unsigned int win_w {static_cast<unsigned int>(wr.right - wr.left)};
	unsigned int win_h {static_cast<unsigned int>(wr.bottom - wr.top)};

	HMONITOR const monitor {
		MonitorFromWindow(handle_, MONITOR_DEFAULTTONEAREST)};
	MONITORINFO mi {sizeof(MONITORINFO)};
	GetMonitorInfo(monitor, &mi);
	LONG const max_w {mi.rcWork.right - mi.rcWork.left};
	LONG const max_h {mi.rcWork.bottom - mi.rcWork.top};

	if (win_w > static_cast<unsigned int>(max_w) &&
		win_h > static_cast<unsigned int>(max_h))
	{
		wants_maximized_ = true;
		return;
	}

	win_w = min(win_w, static_cast<unsigned int>(max_w));
	win_h = min(win_h, static_cast<unsigned int>(max_h));

	LONG const pos_x {mi.rcWork.left + (max_w - static_cast<LONG>(win_w)) / 2};
	LONG const pos_y {mi.rcWork.top + (max_h - static_cast<LONG>(win_h)) / 2};

	SetWindowPos(handle_,
				 nullptr,
				 pos_x,
				 pos_y,
				 static_cast<int>(win_w),
				 static_cast<int>(win_h),
				 SWP_NOZORDER | SWP_NOACTIVATE);
}

void clav::window::load_image(std::wstring const& path)
{
	if (!wic_factory_)
	{
		check_hresult(CoCreateInstance(CLSID_WICImagingFactory,
									   nullptr,
									   CLSCTX_INPROC_SERVER,
									   IID_PPV_ARGS(wic_factory_.put())),
					  "CoCreateInstance(WICImagingFactory)");
	}

	// Only the container header is read here -- GetSize() on the frame does
	// not require decoding pixel data, so this is fast even for very large
	// images. This lets the window get its correct final size and position
	// *before* show(), with no visible resize once the real pixels arrive.
	com_ptr<IWICBitmapDecoder> decoder;
	check_hresult(
		wic_factory_->CreateDecoderFromFilename(path.c_str(),
												nullptr,
												GENERIC_READ,
												WICDecodeMetadataCacheOnDemand,
												decoder.put()),
		"CreateDecoderFromFilename");

	com_ptr<IWICBitmapFrameDecode> frame;
	check_hresult(decoder->GetFrame(0, frame.put()), "GetFrame");

	UINT w, h;
	check_hresult(frame->GetSize(&w, &h), "IWICBitmapFrameDecode::GetSize");
	img_width_ = w;
	img_height_ = h;

	bitmap_.reset();
	last_converter_cache_.reset();

	if (handle_)
	{
		position_for_image(img_width_, img_height_);
	}

	// Bump the generation so any in-flight decode from a previous
	// load_image() call gets ignored when its result arrives.
	unsigned long long const generation {++generation_};

	// Previous worker (if any) is left to finish and post a now-stale
	// result, which WM_IMAGE_DECODED drops via the generation check --
	// detaching avoids blocking this call on however long that decode
	// takes.
	if (decode_thread_.joinable())
	{
		decode_thread_.detach();
	}

	decode_thread_ = std::thread {&clav::window::spawn_decode,
								  handle_,
								  path,
								  generation};
}

void clav::window::spawn_decode(HWND hwnd,
								std::wstring path,
								unsigned long long generation)
{
	// Independent COM apartment + WIC factory for this thread. WIC's
	// factory is safe to share across threads, but decoder/frame objects
	// are not guaranteed to be, so this thread opens the file itself
	// rather than touching any state owned by the UI thread.
	HRESULT const co_hr {CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
	bool const co_owns {SUCCEEDED(co_hr)};

	try
	{
		com_ptr<IWICImagingFactory> factory;
		check_hresult(CoCreateInstance(CLSID_WICImagingFactory,
									   nullptr,
									   CLSCTX_INPROC_SERVER,
									   IID_PPV_ARGS(factory.put())),
					  "CoCreateInstance(WICImagingFactory) [worker]");

		com_ptr<IWICBitmapDecoder> decoder;
		check_hresult(
			factory->CreateDecoderFromFilename(path.c_str(),
											   nullptr,
											   GENERIC_READ,
											   WICDecodeMetadataCacheOnDemand,
											   decoder.put()),
			"CreateDecoderFromFilename [worker]");

		com_ptr<IWICBitmapFrameDecode> frame;
		check_hresult(decoder->GetFrame(0, frame.put()), "GetFrame [worker]");

		// Best-effort embedded thumbnail: many cameras/editors embed a
		// small JPEG preview, which decodes in a fraction of the time the
		// full frame takes. Absence is common and not an error.
		com_ptr<IWICBitmapSource> thumb_source;
		if (SUCCEEDED(frame->GetThumbnail(thumb_source.put())))
		{
			com_ptr<IWICFormatConverter> thumb_converter;
			if (SUCCEEDED(
					factory->CreateFormatConverter(thumb_converter.put())) &&
				SUCCEEDED(thumb_converter->Initialize(
					thumb_source.get(),
					GUID_WICPixelFormat32bppPBGRA,
					WICBitmapDitherTypeNone,
					nullptr,
					0.0,
					WICBitmapPaletteTypeMedianCut)))
			{
				auto* result {new decode_result {std::move(thumb_converter),
												 generation,
												 true}};
				if (!PostMessageW(hwnd,
								  WM_IMAGE_DECODED,
								  0,
								  reinterpret_cast<LPARAM>(result)))
				{
					delete result; // window gone / queue unavailable
				}
			}
		}

		com_ptr<IWICFormatConverter> full_converter;
		check_hresult(factory->CreateFormatConverter(full_converter.put()),
					  "CreateFormatConverter [worker]");
		check_hresult(
			full_converter->Initialize(frame.get(),
									   GUID_WICPixelFormat32bppPBGRA,
									   WICBitmapDitherTypeNone,
									   nullptr,
									   0.0,
									   WICBitmapPaletteTypeMedianCut),
			"IWICFormatConverter::Initialize [worker]");

		auto* result {
			new decode_result {std::move(full_converter), generation, false}};
		if (!PostMessageW(hwnd,
						  WM_IMAGE_DECODED,
						  0,
						  reinterpret_cast<LPARAM>(result)))
		{
			delete result;
		}
	}
	catch (std::exception const&)
	{
		// Decode failures on a background thread have nowhere good to go
		// except being swallowed -- the window simply keeps showing its
		// background color for this image. (A production build would
		// post an error message back to the UI thread here.)
	}

	if (co_owns)
	{
		CoUninitialize();
	}
}

clav::window::window(win_class& owner) : owner_(owner)
{
}

clav::window::~window()
{
	// spawn_decode() is static and only touches a plain HWND value, never
	// `this`, so it's safe to let it keep running after the window object
	// goes away -- detaching avoids blocking destruction on a slow decode.
	if (decode_thread_.joinable())
	{
		decode_thread_.detach();
	}
}

void clav::window::show(int cmd_show)
{
	render();
	ShowWindow(handle_, wants_maximized_ ? SW_MAXIMIZE : cmd_show);
	UpdateWindow(handle_);
}

void clav::window::destroy()
{
	if (handle_)
	{
		DestroyWindow(handle_);
	}
}

clav::win_class::win_class(std::wstring const& cname, HINSTANCE h_instance) :
	name_(cname),
	h_instance_(h_instance)
{
	if (cname.empty())
	{
		throw std::runtime_error("No class name.");
	}
	if (!h_instance)
	{
		throw std::runtime_error("Invalid hInstance.");
	}
	atom_ = register_class();
}

ATOM clav::win_class::register_class()
{
	WNDCLASSEXW const wcexw {.cbSize = sizeof(WNDCLASSEXW),
							 .style = CS_HREDRAW | CS_VREDRAW,
							 .lpfnWndProc = clav::window::s_WindowProc,
							 .cbWndExtra = sizeof(LONG_PTR),
							 .hInstance = h_instance_,
							 .hIcon = LoadIconW(nullptr, IDI_APPLICATION_W),
							 .hCursor = LoadCursorW(nullptr, IDC_ARROW_W),
							 .lpszClassName = name_.c_str()};

	ATOM const atom {RegisterClassExW(&wcexw)};
	if (!atom)
	{
		check_hresult(HRESULT_FROM_WIN32(GetLastError()), "RegisterClassExW");
	}
	return atom;
}

std::unique_ptr<clav::window> clav::win_class::create_window(
	std::wstring const& wname)
{
	auto win {std::make_unique<clav::window>(*this)};

	/* Created with a small placeholder size, hidden. This call is cheap.
	 no GPU/device work happens until the first WM_PAINT after show(). */
	HWND const handle {CreateWindowExW(0,
									   name_.c_str(),
									   wname.c_str(),
									   WS_OVERLAPPEDWINDOW,
									   CW_USEDEFAULT,
									   CW_USEDEFAULT,
									   0,
									   0,
									   nullptr,
									   nullptr,
									   h_instance_,
									   win.get())};

	if (!handle)
	{
		check_hresult(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
	}

	if (icon_)
	{
		SendMessage(handle, WM_SETICON, ICON_SMALL, (LPARAM)icon_);
		SendMessage(handle, WM_SETICON, ICON_BIG, (LPARAM)icon_);
	}

	apply_modern_frame_style(handle);

	return win;
}

void clav::win_class::load_icon()
{
	icon_ = (HICON)LoadImage(h_instance_,
							 MAKEINTRESOURCE(IDI_MY_ICON),
							 IMAGE_ICON,
							 0,
							 0,
							 LR_DEFAULTCOLOR | LR_DEFAULTSIZE);

	if (not icon_)
	{
		check_hresult(HRESULT_FROM_WIN32(GetLastError()), "LoadImage");
	}
}

clav::win_class::~win_class()
{
	if (live_windows_ == 0 && atom_)
	{
		UnregisterClassW(reinterpret_cast<LPCWSTR>(MAKEINTATOM(atom_)),
						 h_instance_);
	}
}
