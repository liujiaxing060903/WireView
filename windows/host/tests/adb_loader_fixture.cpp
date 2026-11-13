#include <iostream>

extern "C" __declspec(dllimport) int wireview_adb_loader_fixture_value();

int main() {
    if (wireview_adb_loader_fixture_value() != 42) { return 1; }
    // A real import-table dependency deliberately named AdbWinApi.dll lets the
    // native session exercise Windows loader failures without a real SDK/phone.
    std::cout << "List of devices attached\n";
    return 0;
}
