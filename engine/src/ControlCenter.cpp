#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace {
constexpr int ID_INPUT = 1001;
constexpr int ID_OUTPUT = 1002;
constexpr int ID_MODE = 1003;
constexpr int ID_APPLY = 1004;
constexpr int ID_START = 1005;
constexpr int ID_STOP = 1006;
constexpr int ID_RESET = 1007;
constexpr int ID_SLIDER_BASE = 1100;
constexpr int ID_VALUE_BASE = 1200;
constexpr UINT_PTR TIMER_ID = 1;

struct AudioDevice {
    std::wstring name;
    std::wstring id;
};
using Kv = std::map<std::string, std::string>;

HWND g_window = nullptr;
HWND g_inputCombo = nullptr;
HWND g_outputCombo = nullptr;
HWND g_modeCombo = nullptr;
HWND g_status = nullptr;
HWND g_startButton = nullptr;
HWND g_stopButton = nullptr;
std::array<HWND, 6> g_sliders{};
std::array<HWND, 6> g_values{};
std::array<int, 6> g_gainDb{{0, 0, 0, 0, 0, 0}};
std::vector<AudioDevice> g_captureDevices;
std::vector<AudioDevice> g_renderDevices;
PROCESS_INFORMATION g_engine{};
std::wstring g_exeDir;
std::wstring g_configPath;
std::wstring g_controlPath;
std::wstring g_logPath;
bool g_initializing = false;

const std::array<const wchar_t*, 6> kChannelNames{{
    L"Front Left (FL)", L"Front Right (FR)", L"Center (C)",
    L"Subwoofer / LFE", L"Surround Left (SL)", L"Surround Right (SR)"
}};
const std::array<const char*, 6> kGainKeys{{"fl_db", "fr_db", "c_db", "lfe_db", "sl_db", "sr_db"}};

std::wstring ExeDirectory() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) return L".";
    path.resize(n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::wstring TrimW(std::wstring s) {
    const auto first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string TrimA(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

Kv ReadKv(const std::wstring& path) {
    Kv values;
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        line = TrimA(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        values[TrimA(line.substr(0, eq))] = TrimA(line.substr(eq + 1));
    }
    return values;
}

bool WriteTextAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream out{std::filesystem::path(temp), std::ios::binary | std::ios::trunc};
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool WriteKvAtomic(const std::wstring& path, const Kv& values) {
    std::ostringstream out;
    for (const auto& item : values) out << item.first << '=' << item.second << "\r\n";
    return WriteTextAtomic(path, out.str());
}

std::vector<AudioDevice> EnumerateDevices(EDataFlow flow) {
    std::vector<AudioDevice> result;
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return result;
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) continue;
            LPWSTR id = nullptr;
            IPropertyStore* store = nullptr;
            std::wstring name = L"(unnamed audio device)";
            std::wstring deviceId;
            if (SUCCEEDED(device->GetId(&id)) && id) deviceId = id;
            if (id) CoTaskMemFree(id);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                    name = pv.pwszVal;
                PropVariantClear(&pv);
                store->Release();
            }
            if (!deviceId.empty()) result.push_back({name, deviceId});
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return result;
}

int FindById(const std::vector<AudioDevice>& devices, const std::string& id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < devices.size(); ++i)
        if (ToUtf8(devices[i].id) == id) return static_cast<int>(i);
    return -1;
}

int FindBySubstring(const std::vector<AudioDevice>& devices, const std::string& text) {
    if (text.empty()) return -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        const std::string name = ToUtf8(devices[i].name);
        if (name.find(text) != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}

int PreferredOutput() {
    for (size_t i = 0; i < g_renderDevices.size(); ++i) {
        std::wstring name = g_renderDevices[i].name;
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (name.find(L"realtek digital") != std::wstring::npos ||
            name.find(L"digital output") != std::wstring::npos ||
            name.find(L"spdif") != std::wstring::npos ||
            name.find(L"s/pdif") != std::wstring::npos)
            return static_cast<int>(i);
    }
    return g_renderDevices.empty() ? -1 : 0;
}

void PopulateDeviceCombo(HWND combo, const std::vector<AudioDevice>& devices) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& device : devices) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
}

void SetStatus(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

bool EngineRunning() {
    if (!g_engine.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(g_engine.hProcess, &code)) return false;
    return code == STILL_ACTIVE;
}

bool WriteControlFile(bool stop) {
    std::ostringstream out;
    out << "stop=" << (stop ? 1 : 0) << "\r\n";
    for (size_t i = 0; i < g_gainDb.size(); ++i)
        out << kGainKeys[i] << '=' << g_gainDb[i] << "\r\n";
    return WriteTextAtomic(g_controlPath, out.str());
}

void UpdateGainLabel(size_t i) {
    if (!g_values[i]) return;
    std::wstring label;
    if (g_gainDb[i] <= -60) label = L"MUTE";
    else label = std::to_wstring(g_gainDb[i]) + L" dB";
    SetWindowTextW(g_values[i], label.c_str());
}

void ReadGainState() {
    std::ifstream in{std::filesystem::path(g_controlPath)};
    std::string line;
    while (std::getline(in, line)) {
        line = TrimA(line);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimA(line.substr(0, eq));
        const std::string value = TrimA(line.substr(eq + 1));
        for (size_t i = 0; i < kGainKeys.size(); ++i) {
            if (key == kGainKeys[i]) {
                try {
                    int db = std::stoi(value);
                    g_gainDb[i] = std::clamp(db, -60, 0);
                } catch (...) {}
            }
        }
    }
}

bool SaveSelectedConfiguration() {
    const LRESULT inSel = SendMessageW(g_inputCombo, CB_GETCURSEL, 0, 0);
    const LRESULT outSel = SendMessageW(g_outputCombo, CB_GETCURSEL, 0, 0);
    if (inSel < 0 || static_cast<size_t>(inSel) >= g_captureDevices.size()) {
        MessageBoxW(g_window, L"Выбери устройство входа (например, CABLE Output).", L"AC3 Control Center", MB_ICONWARNING);
        return false;
    }
    if (outSel < 0 || static_cast<size_t>(outSel) >= g_renderDevices.size()) {
        MessageBoxW(g_window, L"Выбери цифровое устройство вывода, подключённое к ресиверу.", L"AC3 Control Center", MB_ICONWARNING);
        return false;
    }
    Kv cfg = ReadKv(g_configPath);
    if (cfg.empty()) {
        cfg["bitrate"] = "640000";
        cfg["safe"] = "1536";
    }
    cfg["in_id"] = ToUtf8(g_captureDevices[static_cast<size_t>(inSel)].id);
    cfg["out_id"] = ToUtf8(g_renderDevices[static_cast<size_t>(outSel)].id);
    cfg.erase("in");
    cfg.erase("out");
    cfg.erase("out_spdif");
    cfg["loopback"] = "0";
    const LRESULT modeSel = SendMessageW(g_modeCombo, CB_GETCURSEL, 0, 0);
    if (modeSel == 2) {
        cfg["output_mode"] = "pcm";
        cfg["upmix"] = "off";
    } else {
        cfg["output_mode"] = "ac3";
        cfg["upmix"] = modeSel == 0 ? "off" : "surround";
    }
    if (!WriteKvAtomic(g_configPath, cfg)) {
        MessageBoxW(g_window, L"Не удалось записать virtual-ac3-encoder.conf. Проверь права доступа к папке.", L"AC3 Control Center", MB_ICONERROR);
        return false;
    }
    if (!WriteControlFile(false)) {
        MessageBoxW(g_window, L"Не удалось записать ac3-control.ini.", L"AC3 Control Center", MB_ICONERROR);
        return false;
    }
    return true;
}

void CloseProcessHandles() {
    if (g_engine.hThread) CloseHandle(g_engine.hThread);
    if (g_engine.hProcess) CloseHandle(g_engine.hProcess);
    ZeroMemory(&g_engine, sizeof(g_engine));
}

bool StartEngine() {
    if (EngineRunning()) {
        SetStatus(L"Кодировщик уже работает.");
        return true;
    }
    CloseProcessHandles();
    if (!SaveSelectedConfiguration()) return false;
    const std::wstring enginePath = g_exeDir + L"\\engine.exe";
    const DWORD attrs = GetFileAttributesW(enginePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_window, L"Не найден engine.exe рядом с Control Center. Сначала собери проект CMake.", L"AC3 Control Center", MB_ICONERROR);
        return false;
    }
    std::wstring command = L"\"" + enginePath + L"\" --hidden --log \"" + g_logPath + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(enginePath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, g_exeDir.c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        std::wstring message = L"Не удалось запустить engine.exe. Код Windows: " + std::to_wstring(err);
        MessageBoxW(g_window, message.c_str(), L"AC3 Control Center", MB_ICONERROR);
        return false;
    }
    g_engine = pi;
    CloseHandle(g_engine.hThread);
    g_engine.hThread = nullptr;
    SetStatus(L"Запуск кодировщика… подробности: engine.log");
    return true;
}

void StopEngine(bool updateStatus = true) {
    if (!g_engine.hProcess) {
        WriteControlFile(false);
        if (updateStatus) SetStatus(L"Кодировщик остановлен.");
        return;
    }
    if (EngineRunning()) {
        WriteControlFile(true);
        DWORD wait = WaitForSingleObject(g_engine.hProcess, 3000);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(g_engine.hProcess, 3);
            WaitForSingleObject(g_engine.hProcess, 1500);
            if (updateStatus) SetStatus(L"Движок не ответил на штатную остановку; процесс завершён принудительно.");
        }
    }
    CloseProcessHandles();
    WriteControlFile(false);
    if (updateStatus) SetStatus(L"Кодировщик остановлен.");
}

void ApplyConfiguration() {
    if (!SaveSelectedConfiguration()) return;
    if (EngineRunning()) {
        const int answer = MessageBoxW(g_window,
            L"Изменение режима или устройств требует перезапуска кодировщика. Звук кратковременно прервётся. Перезапустить сейчас?",
            L"Применить настройки", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            SetStatus(L"Настройки сохранены; перезапуск не выполнен.");
            return;
        }
        StopEngine(false);
        if (StartEngine()) SetStatus(L"Настройки применены, кодировщик перезапущен.");
    } else {
        SetStatus(L"Настройки сохранены. Нажми «Запустить» для применения.");
    }
}

void UpdateModeControls() {
    if (!g_modeCombo) return;
    const bool pcmMode = SendMessageW(g_modeCombo, CB_GETCURSEL, 0, 0) == 2;
    for (size_t i = 0; i < g_sliders.size(); ++i)
        EnableWindow(g_sliders[i], !pcmMode || i < 2);
    if (pcmMode)
        SetStatus(L"PCM Stereo 2.0: используются только левый и правый каналы.");
}

void ResetGains() {
    g_initializing = true;
    for (size_t i = 0; i < g_sliders.size(); ++i) {
        g_gainDb[i] = 0;
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, 0);
        UpdateGainLabel(i);
    }
    g_initializing = false;
    WriteControlFile(false);
    SetStatus(EngineRunning() ? L"Все уровни сброшены на 0 dB и применены." : L"Уровни сброшены на 0 dB.");
}

void CheckEngineStatus() {
    if (!g_engine.hProcess) return;
    DWORD code = 0;
    if (GetExitCodeProcess(g_engine.hProcess, &code) && code != STILL_ACTIVE) {
        CloseProcessHandles();
        SetStatus(L"Движок остановлен (код завершения " + std::to_wstring(code) + L"). Подробности: engine.log");
    }
}

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id, HWND parent) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void CreateControls(HWND hwnd) {
    AddControl(L"STATIC", L"Входное устройство (WASAPI capture):", 0, 20, 18, 260, 24, 0, hwnd);
    g_inputCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 290, 16, 560, 240, ID_INPUT, hwnd);
    PopulateDeviceCombo(g_inputCombo, g_captureDevices);

    AddControl(L"STATIC", L"Цифровой выход (S/PDIF / Toslink):", 0, 20, 55, 260, 24, 0, hwnd);
    g_outputCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 290, 52, 560, 240, ID_OUTPUT, hwnd);
    PopulateDeviceCombo(g_outputCombo, g_renderDevices);

    AddControl(L"STATIC", L"Формат цифрового выхода:", 0, 20, 93, 230, 24, 0, hwnd);
    g_modeCombo = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 290, 89, 270, 220, ID_MODE, hwnd);
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dolby Digital 5.1 — без upmix"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dolby Digital 5.1 — Surround upmix"));
    SendMessageW(g_modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PCM Stereo 2.0 — обычный S/PDIF"));
    AddControl(L"BUTTON", L"Применить / перезапустить", BS_PUSHBUTTON | WS_TABSTOP, 575, 88, 275, 30, ID_APPLY, hwnd);

    AddControl(L"STATIC", L"Громкость каналов (−60 dB = MUTE; 0 dB = исходный уровень). Ползунки применяются на лету.", 0, 20, 130, 830, 24, 0, hwnd);
    for (size_t i = 0; i < g_sliders.size(); ++i) {
        const int y = 166 + static_cast<int>(i) * 45;
        AddControl(L"STATIC", kChannelNames[i], 0, 20, y + 3, 255, 24, 0, hwnd);
        g_sliders[i] = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                                  290, y, 460, 32, ID_SLIDER_BASE + static_cast<int>(i), hwnd);
        SendMessageW(g_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(-60, 0));
        SendMessageW(g_sliders[i], TBM_SETTICFREQ, 10, 0);
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, g_gainDb[i]);
        g_values[i] = AddControl(L"STATIC", L"0 dB", SS_RIGHT, 765, y + 5, 80, 22, ID_VALUE_BASE + static_cast<int>(i), hwnd);
        UpdateGainLabel(i);
    }
    AddControl(L"BUTTON", L"Сбросить все уровни", BS_PUSHBUTTON | WS_TABSTOP, 290, 445, 190, 32, ID_RESET, hwnd);
    g_startButton = AddControl(L"BUTTON", L"Запустить кодировщик", BS_PUSHBUTTON | WS_TABSTOP, 20, 495, 220, 36, ID_START, hwnd);
    g_stopButton = AddControl(L"BUTTON", L"Остановить", BS_PUSHBUTTON | WS_TABSTOP, 255, 495, 180, 36, ID_STOP, hwnd);
    g_status = AddControl(L"STATIC", L"Готов. Кодировщик остановлен.", 0, 20, 545, 830, 38, 0, hwnd);

    Kv cfg = ReadKv(g_configPath);
    int inIndex = FindById(g_captureDevices, cfg["in_id"]);
    if (inIndex < 0) inIndex = FindBySubstring(g_captureDevices, cfg["in"]);
    if (inIndex < 0) inIndex = FindBySubstring(g_captureDevices, "CABLE Output");
    if (inIndex < 0 && !g_captureDevices.empty()) inIndex = 0;
    int outIndex = FindById(g_renderDevices, cfg["out_id"]);
    if (outIndex < 0) outIndex = FindBySubstring(g_renderDevices, cfg["out"]);
    if (outIndex < 0) outIndex = PreferredOutput();
    if (inIndex >= 0) SendMessageW(g_inputCombo, CB_SETCURSEL, inIndex, 0);
    if (outIndex >= 0) SendMessageW(g_outputCombo, CB_SETCURSEL, outIndex, 0);
    const LRESULT selectedMode = cfg["output_mode"] == "pcm" ? 2 : (cfg["upmix"] == "off" ? 0 : 1);
    SendMessageW(g_modeCombo, CB_SETCURSEL, selectedMode, 0);
    UpdateModeControls();
    EnableWindow(g_startButton, !g_captureDevices.empty() && !g_renderDevices.empty());
    EnableWindow(g_stopButton, FALSE);
    SetTimer(hwnd, TIMER_ID, 500, nullptr);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_MODE && HIWORD(wParam) == CBN_SELCHANGE) {
            UpdateModeControls();
            const LRESULT mode = SendMessageW(g_modeCombo, CB_GETCURSEL, 0, 0);
            if (mode == 2)
                SetStatus(L"Выбран PCM Stereo 2.0. Нажми «Применить / перезапустить» для переключения формата.");
            else
                SetStatus(L"Выбран выход Dolby Digital 5.1. Нажми «Применить / перезапустить» для переключения формата.");
            return 0;
        }
        if (id == ID_START && HIWORD(wParam) == BN_CLICKED) {
            if (StartEngine()) {
                EnableWindow(g_startButton, FALSE);
                EnableWindow(g_stopButton, TRUE);
            }
            return 0;
        }
        if (id == ID_STOP && HIWORD(wParam) == BN_CLICKED) {
            StopEngine();
            EnableWindow(g_startButton, TRUE);
            EnableWindow(g_stopButton, FALSE);
            return 0;
        }
        if (id == ID_APPLY && HIWORD(wParam) == BN_CLICKED) {
            ApplyConfiguration();
            const bool running = EngineRunning();
            EnableWindow(g_startButton, !running);
            EnableWindow(g_stopButton, running);
            return 0;
        }
        if (id == ID_RESET && HIWORD(wParam) == BN_CLICKED) {
            ResetGains();
            return 0;
        }
        return 0;
    }
    case WM_HSCROLL: {
        if (g_initializing) return 0;
        HWND slider = reinterpret_cast<HWND>(lParam);
        for (size_t i = 0; i < g_sliders.size(); ++i) {
            if (slider == g_sliders[i]) {
                g_gainDb[i] = static_cast<int>(SendMessageW(g_sliders[i], TBM_GETPOS, 0, 0));
                UpdateGainLabel(i);
                WriteControlFile(false);
                SetStatus(EngineRunning() ? L"Уровни каналов применяются в реальном времени." : L"Уровни сохранены; будут использованы при запуске.");
                break;
            }
        }
        return 0;
    }
    case WM_TIMER:
        CheckEngineStatus();
        if (g_startButton && g_stopButton) {
            const bool running = EngineRunning();
            EnableWindow(g_startButton, !running);
            EnableWindow(g_stopButton, running);
        }
        return 0;
    case WM_CLOSE:
        StopEngine(false);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitializeCom = SUCCEEDED(comHr);

    g_exeDir = ExeDirectory();
    g_configPath = g_exeDir + L"\\virtual-ac3-encoder.conf";
    g_controlPath = g_exeDir + L"\\ac3-control.ini";
    g_logPath = g_exeDir + L"\\engine.log";
    g_captureDevices = EnumerateDevices(eCapture);
    g_renderDevices = EnumerateDevices(eRender);
    ReadGainState();

    const wchar_t CLASS_NAME[] = L"Ac3ControlCenterWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&wc)) {
        if (uninitializeCom) CoUninitialize();
        return 1;
    }
    g_window = CreateWindowExW(0, CLASS_NAME, L"AC3 Control Center — Dolby Digital / PCM Stereo",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        if (uninitializeCom) CoUninitialize();
        return 1;
    }
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (uninitializeCom) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
