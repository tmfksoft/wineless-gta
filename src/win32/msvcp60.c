// msvcp60: the sliver of the MSVC 6 C++ runtime (msvcp60.dll) that games touch.
// Morrowind and its launcher include <iostream>, which emits a static
// std::ios_base::Init object; its constructor/destructor would set up the
// standard streams (cout/cerr/clog/cin). A program that only links iostreams,
// without actually using those streams, runs fine with these as no-ops.
#include "win32.h"

// std::ios_base::Init::Init() / ::~Init() — thiscall, no stack arguments.
API_N(msvcp60, ios_base_Init_ctor, "??0Init@ios_base@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, ios_base_Init_dtor, "??1Init@ios_base@std@@QAE@XZ", 0) { return 0; }
// std::_Winit::_Winit() / ::~_Winit() — the wide-stream (wcout/wcin) counterpart.
API_N(msvcp60, Winit_ctor, "??0_Winit@std@@QAE@XZ", 0) { return 0; }
API_N(msvcp60, Winit_dtor, "??1_Winit@std@@QAE@XZ", 0) { return 0; }
