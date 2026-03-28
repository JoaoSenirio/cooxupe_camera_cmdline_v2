#include "ui_engine.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include "thread_queue.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

struct UiEngine::Impl {
    explicit Impl(CommandSink sink) : command_sink(std::move(sink)) {}

    CommandSink command_sink;
    ThreadQueue<UiEvent> events{256};
    std::thread thread;
    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool start_complete = false;
    bool start_ok = false;
    bool stopping = false;

#ifdef _WIN32
    static constexpr UINT kMsgDrainEvents = WM_APP + 1;
    static constexpr UINT kMsgTrayIcon = WM_APP + 2;
    static constexpr UINT_PTR kTimerAutoHide = 1;
    static constexpr int kCommandStatus = 1001;
    static constexpr int kCommandExit = 1002;
    static constexpr int kButtonExit = 1003;

    HWND host_window = nullptr;
    HWND popup_window = nullptr;
    HWND title_label = nullptr;
    HWND detail_label = nullptr;
    HWND eta_label = nullptr;
    HWND progress_bar = nullptr;
    HWND exit_button = nullptr;
    NOTIFYICONDATAA tray_icon{};
    UiEvent last_event{};
    bool has_last_event = false;

    static LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        UiEngine::Impl* self = reinterpret_cast<UiEngine::Impl*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            CREATESTRUCTA* create = reinterpret_cast<CREATESTRUCTA*>(lparam);
            self = reinterpret_cast<UiEngine::Impl*>(create->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if (self == nullptr) {
            return DefWindowProcA(hwnd, message, wparam, lparam);
        }

        switch (message) {
            case kMsgDrainEvents:
                self->DrainEvents();
                return 0;
            case kMsgTrayIcon:
                if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
                    self->ShowTrayMenu();
                } else if (lparam == WM_LBUTTONDBLCLK) {
                    self->ShowLastPopup();
                }
                return 0;
            case WM_COMMAND:
                if (LOWORD(wparam) == kCommandStatus) {
                    self->ShowLastPopup();
                    return 0;
                }
                if (LOWORD(wparam) == kCommandExit) {
                    self->RequestExit();
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }

        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK PopupWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        UiEngine::Impl* self = reinterpret_cast<UiEngine::Impl*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            CREATESTRUCTA* create = reinterpret_cast<CREATESTRUCTA*>(lparam);
            self = reinterpret_cast<UiEngine::Impl*>(create->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if (self == nullptr) {
            return DefWindowProcA(hwnd, message, wparam, lparam);
        }

        switch (message) {
            case WM_COMMAND:
                if (LOWORD(wparam) == kButtonExit) {
                    self->RequestExit();
                    return 0;
                }
                break;
            case WM_TIMER:
                if (wparam == kTimerAutoHide) {
                    KillTimer(hwnd, kTimerAutoHide);
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                }
                break;
            case WM_CLOSE:
                return 0;
        }

        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    bool Start() {
        thread = std::thread([this]() { ThreadMain(); });

        std::unique_lock<std::mutex> lock(start_mutex);
        start_cv.wait(lock, [this]() { return start_complete; });
        return start_ok;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(start_mutex);
            stopping = true;
        }

        if (host_window != nullptr) {
            PostMessageA(host_window, WM_CLOSE, 0, 0);
        }
        events.close();
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool Publish(const UiEvent& event) {
        if (!events.push(event)) {
            return false;
        }
        if (host_window != nullptr) {
            PostMessageA(host_window, kMsgDrainEvents, 0, 0);
        }
        return true;
    }

    void ThreadMain() {
        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&controls);

        WNDCLASSA host_class{};
        host_class.lpfnWndProc = &UiEngine::Impl::HostWindowProc;
        host_class.hInstance = GetModuleHandleA(nullptr);
        host_class.lpszClassName = "SpecSensorCliUiHostWindow";
        host_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&host_class);

        WNDCLASSA popup_class{};
        popup_class.lpfnWndProc = &UiEngine::Impl::PopupWindowProc;
        popup_class.hInstance = GetModuleHandleA(nullptr);
        popup_class.lpszClassName = "SpecSensorCliUiPopupWindow";
        popup_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        popup_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassA(&popup_class);

        host_window = CreateWindowExA(0,
                                      host_class.lpszClassName,
                                      "SpecSensorCliHost",
                                      WS_OVERLAPPED,
                                      0,
                                      0,
                                      0,
                                      0,
                                      nullptr,
                                      nullptr,
                                      host_class.hInstance,
                                      this);

        if (host_window != nullptr) {
            CreatePopupWindow();
            AddTrayIcon();
        }

        {
            std::lock_guard<std::mutex> lock(start_mutex);
            start_ok = host_window != nullptr && popup_window != nullptr;
            start_complete = true;
        }
        start_cv.notify_all();

        if (!start_ok) {
            return;
        }

        MSG message{};
        while (GetMessageA(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }

        RemoveTrayIcon();
        if (popup_window != nullptr) {
            DestroyWindow(popup_window);
            popup_window = nullptr;
        }
        if (host_window != nullptr) {
            DestroyWindow(host_window);
            host_window = nullptr;
        }
    }

    void CreatePopupWindow() {
        popup_window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                       "SpecSensorCliUiPopupWindow",
                                       "SpecSensorCliPopup",
                                       WS_POPUP | WS_BORDER,
                                       CW_USEDEFAULT,
                                       CW_USEDEFAULT,
                                       520,
                                       220,
                                       nullptr,
                                       nullptr,
                                       GetModuleHandleA(nullptr),
                                       this);
        if (popup_window == nullptr) {
            return;
        }

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        title_label = CreateWindowExA(0, "STATIC", "",
                                      WS_CHILD | WS_VISIBLE,
                                      24, 24, 460, 24,
                                      popup_window, nullptr, GetModuleHandleA(nullptr), nullptr);
        detail_label = CreateWindowExA(0, "STATIC", "",
                                       WS_CHILD | WS_VISIBLE,
                                       24, 58, 460, 36,
                                       popup_window, nullptr, GetModuleHandleA(nullptr), nullptr);
        progress_bar = CreateWindowExA(0, PROGRESS_CLASSA, "",
                                       WS_CHILD | WS_VISIBLE,
                                       24, 110, 460, 24,
                                       popup_window, nullptr, GetModuleHandleA(nullptr), nullptr);
        eta_label = CreateWindowExA(0, "STATIC", "",
                                    WS_CHILD | WS_VISIBLE,
                                    24, 146, 300, 24,
                                    popup_window, nullptr, GetModuleHandleA(nullptr), nullptr);
        exit_button = CreateWindowExA(0, "BUTTON", "Encerrar",
                                      WS_CHILD | BS_PUSHBUTTON,
                                      384, 146, 100, 28,
                                      popup_window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonExit)),
                                      GetModuleHandleA(nullptr),
                                      nullptr);

        SendMessageA(title_label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageA(detail_label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageA(eta_label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageA(exit_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageA(progress_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        ShowWindow(exit_button, SW_HIDE);
    }

    void AddTrayIcon() {
        tray_icon = {};
        tray_icon.cbSize = sizeof(tray_icon);
        tray_icon.hWnd = host_window;
        tray_icon.uID = 1;
        tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_icon.uCallbackMessage = kMsgTrayIcon;
        tray_icon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        lstrcpynA(tray_icon.szTip, "SpecSensor Camera Background", sizeof(tray_icon.szTip));
        Shell_NotifyIconA(NIM_ADD, &tray_icon);
    }

    void RemoveTrayIcon() {
        if (tray_icon.hWnd != nullptr) {
            Shell_NotifyIconA(NIM_DELETE, &tray_icon);
            tray_icon = {};
        }
    }

    void RequestExit() {
        if (command_sink) {
            command_sink(UiCommand{UiCommandType::ExitRequested});
        }
    }

    void ShowTrayMenu() {
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(host_window);

        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, kCommandStatus, "Status");
        AppendMenuA(menu, MF_STRING, kCommandExit, "Encerrar");
        const UINT flags = TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD;
        const UINT command = TrackPopupMenu(menu, flags, point.x, point.y, 0, host_window, nullptr);
        DestroyMenu(menu);

        if (command == kCommandStatus) {
            ShowLastPopup();
        } else if (command == kCommandExit) {
            RequestExit();
        }
    }

    void ShowLastPopup() {
        if (has_last_event) {
            ApplyEvent(last_event);
        }
    }

    void DrainEvents() {
        UiEvent event;
        while (events.size() > 0 && events.pop(&event)) {
            ApplyEvent(event);
        }
    }

    void ApplyEvent(const UiEvent& event) {
        last_event = event;
        has_last_event = event.type != UiEventType::Hide && event.type != UiEventType::Shutdown;

        if (popup_window == nullptr) {
            return;
        }

        if (event.type == UiEventType::Hide || event.type == UiEventType::Shutdown) {
            KillTimer(popup_window, kTimerAutoHide);
            ShowWindow(popup_window, SW_HIDE);
            return;
        }

        SetWindowTextA(title_label, event.title.c_str());
        SetWindowTextA(detail_label, event.detail.c_str());
        if (event.eta_seconds >= 0) {
            const std::string eta_text = "Tempo estimado restante: " + std::to_string(event.eta_seconds) + " s";
            SetWindowTextA(eta_label, eta_text.c_str());
        } else {
            SetWindowTextA(eta_label, "");
        }
        SendMessageA(progress_bar, PBM_SETPOS, static_cast<WPARAM>(event.progress_percent), 0);

        const bool show_exit_button = event.type == UiEventType::Error;
        ShowWindow(exit_button, show_exit_button ? SW_SHOW : SW_HIDE);

        const int width = 520;
        const int height = show_exit_button ? 220 : 190;
        const int screen_width = GetSystemMetrics(SM_CXSCREEN);
        const int screen_height = GetSystemMetrics(SM_CYSCREEN);
        const int x = (screen_width - width) / 2;
        const int y = (screen_height - height) / 2;

        SetWindowPos(popup_window,
                     HWND_TOPMOST,
                     x,
                     y,
                     width,
                     height,
                     SWP_SHOWWINDOW);
        ShowWindow(popup_window, SW_SHOWNORMAL);
        UpdateWindow(popup_window);

        if (show_exit_button) {
            SetForegroundWindow(popup_window);
            SetFocus(exit_button);
        }

        KillTimer(popup_window, kTimerAutoHide);
        if (event.auto_hide_delay_ms > 0) {
            SetTimer(popup_window,
                     kTimerAutoHide,
                     static_cast<UINT>(event.auto_hide_delay_ms),
                     nullptr);
        }
    }
#else
    bool Start() {
        start_complete = true;
        start_ok = true;
        return true;
    }

    void Stop() {
        events.close();
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool Publish(const UiEvent&) {
        return true;
    }
#endif
};

UiEngine::UiEngine() = default;

UiEngine::~UiEngine() {
    stop();
}

bool UiEngine::start(CommandSink command_sink) {
    if (impl_) {
        return true;
    }

    impl_.reset(new Impl(std::move(command_sink)));
    if (!impl_->Start()) {
        impl_.reset();
        return false;
    }
    return true;
}

void UiEngine::stop() {
    if (!impl_) {
        return;
    }
    impl_->Stop();
    impl_.reset();
}

bool UiEngine::publish(const UiEvent& event) {
    if (!impl_) {
        return false;
    }
    return impl_->Publish(event);
}
