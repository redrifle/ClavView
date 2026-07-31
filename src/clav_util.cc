#include "clav_util.hpp"
#include <format>
#include <stdexcept>

std::wstring to_wide(std::string_view const s)
{
	if (s.empty())
	{
		return L"";
	}

	int const wlen {MultiByteToWideChar(CP_UTF8,
										0,
										s.data(),
										static_cast<int>(s.size()),
										nullptr,
										0)};

	if (wlen == 0)
	{
		throw std::runtime_error("Couldn't get multibyte string length");
	}

	std::wstring ws(wlen, L'\0');
	int const rv {MultiByteToWideChar(CP_UTF8,
									  0,
									  s.data(),
									  static_cast<int>(s.size()),
									  ws.data(),
									  wlen)};

	if (rv == 0)
	{
		throw std::runtime_error("Couldn't convert to wide string");
	}

	return ws;
}

std::string myerr(DWORD const err)
{
	LPSTR msg {nullptr};
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
					   FORMAT_MESSAGE_FROM_SYSTEM |
					   FORMAT_MESSAGE_IGNORE_INSERTS,
				   NULL,
				   err,
				   0,
				   reinterpret_cast<LPSTR>(&msg),
				   0,
				   nullptr);

	std::string result {msg ? msg : "Unknown error"};
	if (msg)
	{
		LocalFree(msg);
	}
	return result;
}

std::wstring mywerr(DWORD const err)
{
	LPWSTR wmsg {nullptr};
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
					   FORMAT_MESSAGE_FROM_SYSTEM |
					   FORMAT_MESSAGE_IGNORE_INSERTS,
				   NULL,
				   err,
				   0,
				   reinterpret_cast<LPWSTR>(&wmsg),
				   0,
				   nullptr);

	std::wstring result {wmsg ? wmsg : L"Unknown error"};
	if (wmsg)
	{
		LocalFree(wmsg);
	}
	return result;
}

void check_hresult(HRESULT hr, char const* what)
{
	if (FAILED(hr))
	{
		throw std::runtime_error(
			std::format("{} failed (0x{:08X}): {}",
						what,
						static_cast<unsigned long>(hr),
						myerr(static_cast<DWORD>(hr))));
	}
}
