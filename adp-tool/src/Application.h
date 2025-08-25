#pragma once

namespace adp {
    class Application {
    public:
        Application();
        ~Application();

        [[noreturn]] void UpdateLoop();
    private:
        static void OnInit();
        static void OnExit();
        static void Tick();
    };
}
