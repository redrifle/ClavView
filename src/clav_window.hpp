#ifndef WIN32_WINDOW_HPP_019f7b20_37b2_7245_b65e_7808f9eac4b0
#define WIN32_WINDOW_HPP_019f7b20_37b2_7245_b65e_7808f9eac4b0
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h> // D2D1::RectF, D2D1::SizeU, D2D1::ColorF, etc.
#include <wincodec.h>
#include "com_ptr.hpp"

namespace clav
{
	class win_class
	{
		friend class window;
		ATOM atom_ {};
		std::wstring name_;
		HINSTANCE h_instance_ {};
		unsigned int live_windows_ {0};
		ATOM register_class();
		HICON icon_;

	  public:
		std::unique_ptr<window> create_window(std::wstring const&);
		void load_icon();
		win_class(std::wstring const&, HINSTANCE);
		~win_class();
	};

	class window
	{
		HWND handle_ {nullptr};
		win_class& owner_;
		bool wants_maximized_ {false};

		// D2D factory is process-wide and cheap to create; kept per-window
		// here for simplicity, but never touches the GPU until first use.
		com_ptr<ID2D1Factory> d2d_factory_;
		com_ptr<IWICImagingFactory> wic_factory_;

		// The most recently applied decoded frame (thumbnail or full-res),
		// kept around purely so a lost D2D device can be rebuilt from it
		// without re-decoding or re-reading the source file.
		com_ptr<IWICFormatConverter> last_converter_cache_;

		// GPU-side resources. Created lazily on first WM_PAINT, not on load,
		// so ShowWindow() happens before any GPU work is touched.
		com_ptr<ID2D1HwndRenderTarget> render_target_;
		com_ptr<ID2D1Bitmap> bitmap_;

		unsigned int img_width_ {0}, img_height_ {0};
		bool in_sizemove_ {false};

		// Guards against a background decode finishing after a newer
		// load_image() call has superseded it -- results are tagged with
		// the generation active when they were requested and dropped if
		// it no longer matches when they arrive.
		std::atomic<unsigned long long> generation_ {0};
		std::thread decode_thread_;

		RECT calculate_window_rect(unsigned int client_w,
								   unsigned int client_h) const;
		void position_for_image(unsigned int native_w, unsigned int native_h);
		void ensure_device_resources();
		void discard_device_resources();
		void render();

		// Runs on a background thread. Deliberately static and takes only
		// a plain HWND (not `this`/handle_) so it never touches the window
		// object itself -- safe even if the window is destroyed while a
		// decode is still in flight.
		static void spawn_decode(HWND hwnd,
								 std::wstring path,
								 unsigned long long generation);

	  public:
		window(win_class& owner);
		~window();

		static LRESULT CALLBACK s_WindowProc(HWND, UINT, WPARAM, LPARAM);
		LRESULT WindowProc(HWND, UINT, WPARAM, LPARAM);

		void load_image(std::wstring const& path);
		void show(int cmd_show);
		void destroy();

		HWND handle() const
		{
			return handle_;
		}
	};

} /* namespace clav */
#endif /* WIN32_WINDOW_HPP_019f7b20_37b2_7245_b65e_7808f9eac4b0 */
