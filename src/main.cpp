#include <windows.h>

#include "ui/application.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    ui::Application application;
    if (!application.Initialize(instance, showCommand)) {
        return 1;
    }
    return application.Run();
}
