// A mapped managed DLL with no other dependency; used by the native controller
// from a process outside the installation to exercise Win32 image file locks.
extern "C" __declspec(dllexport) int seriousshot_update_lock_fixture() { return 1; }
