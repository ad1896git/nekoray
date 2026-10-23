// Keep main.cpp's ordinary globals and functions for production UI objects,
// without running application startup from the persistence tests.
#define main nkr_unused_application_main
#include "../main/main.cpp"
#undef main
