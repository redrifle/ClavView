add_executable(clav clav_main.cc
    clav_window.cc
    clav_window.hpp
    clav_util.cc
    clav_util.hpp
    project.rc
    resource.h)
set_target_properties(clav PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(clav PRIVATE user32 gdi32 kernel32 gdiplus shlwapi d2d1.lib windowscodecs.lib windowsapp.lib ole32.lib)
