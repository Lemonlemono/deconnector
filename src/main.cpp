#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <fwpmu.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <tlhelp32.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "fwpuclnt.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "xinput.lib")

namespace {

constexpr int kHotkeyId = 2001;
constexpr UINT kBlockDoneMessage = WM_APP + 1;
constexpr UINT kBlockFailedMessage = WM_APP + 2;
constexpr UINT_PTR kCountdownTimer = 3001;
constexpr UINT_PTR kGamepadPollTimer = 3002;
constexpr UINT_PTR kProcessRefreshTimer = 3003;
constexpr int kMinDurationSeconds = 1;
constexpr int kMaxDurationSeconds = 300;
constexpr int kDefaultDurationSeconds = 5;

constexpr int kListProcesses = 101;
constexpr int kButtonRefresh = 102;
constexpr int kButtonBindHotkey = 103;
constexpr int kButtonDisconnect = 104;
constexpr int kStaticTarget = 105;
constexpr int kStaticHotkey = 106;
constexpr int kStaticStatus = 107;
constexpr int kStaticDuration = 108;
constexpr int kEditDuration = 109;
constexpr int kSpinDuration = 110;
constexpr int kCheckLockTarget = 111;

const GUID kSublayerKey = {
    0x5a93bc98, 0xe189, 0x4718, {0x90, 0xd2, 0x79, 0x9a, 0xc0, 0x7e, 0x9c, 0xd1}
};

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;
};

enum class BindingKind {
    Keyboard = 0,
    Gamepad = 1,
    RawGamepad = 2
};

struct InputBinding {
    BindingKind kind = BindingKind::Keyboard;
    UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    UINT vk = VK_F8;
    WORD gamepadButtons = XINPUT_GAMEPAD_A;
    std::wstring rawGamepadDevice;
    uint64_t rawGamepadButtons = 0;
};

HWND g_list = nullptr;
HWND g_refreshButton = nullptr;
HWND g_bindButton = nullptr;
HWND g_disconnectButton = nullptr;
HWND g_targetLabel = nullptr;
HWND g_hotkeyLabel = nullptr;
HWND g_statusLabel = nullptr;
HWND g_durationLabel = nullptr;
HWND g_durationEdit = nullptr;
HWND g_durationSpin = nullptr;
HWND g_lockTargetCheck = nullptr;

std::vector<ProcessInfo> g_processes;
std::wstring g_configPath;
std::wstring g_targetPath;
std::wstring g_targetName;
InputBinding g_binding;
bool g_capturingHotkey = false;
bool g_targetLocked = true;
int g_durationSeconds = kDefaultDurationSeconds;
WORD g_previousGamepadButtons[XUSER_MAX_COUNT]{};
std::map<std::wstring, uint64_t> g_previousRawGamepadButtons;
std::atomic_bool g_blockActive = false;
std::chrono::steady_clock::time_point g_blockEndsAt;

std::wstring FormatWin32Error(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"success";
    }

    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message = length && buffer ? buffer : L"Unknown error";
    if (buffer) {
        LocalFree(buffer);
    }

    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring GetLocalAppDataPath() {
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    }
    return buffer;
}

void EnsureDirectory(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
}

std::wstring GetConfigPath() {
    std::wstring dir = GetLocalAppDataPath() + L"\\Deconnector";
    EnsureDirectory(dir);
    return dir + L"\\config.ini";
}

std::wstring QueryProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return L"";
    }

    wchar_t path[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result.assign(path, size);
    }
    CloseHandle(process);
    return result;
}

std::vector<ProcessInfo> EnumerateProcesses() {
    std::vector<ProcessInfo> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessInfo info;
            info.pid = entry.th32ProcessID;
            info.name = entry.szExeFile;
            info.path = QueryProcessPath(info.pid);
            if (!info.path.empty()) {
                result.push_back(std::move(info));
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    std::sort(result.begin(), result.end(), [](const ProcessInfo& left, const ProcessInfo& right) {
        int nameCompare = _wcsicmp(left.name.c_str(), right.name.c_str());
        if (nameCompare != 0) {
            return nameCompare < 0;
        }
        return left.pid < right.pid;
    });

    return result;
}

std::wstring KeyName(UINT vk) {
    if (vk >= L'A' && vk <= L'Z') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= L'0' && vk <= L'9') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    }

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_END || vk == VK_HOME ||
        vk == VK_INSERT || vk == VK_DELETE) {
        scan |= 0x100;
    }

    wchar_t buffer[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), buffer, static_cast<int>(std::size(buffer))) > 0) {
        return buffer;
    }
    return L"VK " + std::to_wstring(vk);
}

std::wstring KeyboardBindingText(const InputBinding& binding) {
    std::wstring text;
    if (binding.modifiers & MOD_CONTROL) {
        text += L"Ctrl + ";
    }
    if (binding.modifiers & MOD_ALT) {
        text += L"Alt + ";
    }
    if (binding.modifiers & MOD_SHIFT) {
        text += L"Shift + ";
    }
    if (binding.modifiers & MOD_WIN) {
        text += L"Win + ";
    }
    text += KeyName(binding.vk);
    return text;
}

std::wstring GamepadButtonName(WORD button) {
    switch (button) {
    case XINPUT_GAMEPAD_DPAD_UP: return L"DPad Up";
    case XINPUT_GAMEPAD_DPAD_DOWN: return L"DPad Down";
    case XINPUT_GAMEPAD_DPAD_LEFT: return L"DPad Left";
    case XINPUT_GAMEPAD_DPAD_RIGHT: return L"DPad Right";
    case XINPUT_GAMEPAD_START: return L"Start";
    case XINPUT_GAMEPAD_BACK: return L"Back";
    case XINPUT_GAMEPAD_LEFT_THUMB: return L"Left Stick";
    case XINPUT_GAMEPAD_RIGHT_THUMB: return L"Right Stick";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return L"LB";
    case XINPUT_GAMEPAD_RIGHT_SHOULDER: return L"RB";
    case XINPUT_GAMEPAD_A: return L"A";
    case XINPUT_GAMEPAD_B: return L"B";
    case XINPUT_GAMEPAD_X: return L"X";
    case XINPUT_GAMEPAD_Y: return L"Y";
    default: return L"Button " + std::to_wstring(button);
    }
}

std::wstring GamepadBindingText(WORD buttons) {
    constexpr WORD knownButtons[] = {
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y
    };

    std::wstring text;
    for (WORD button : knownButtons) {
        if (buttons & button) {
            if (!text.empty()) {
                text += L" + ";
            }
            text += GamepadButtonName(button);
        }
    }

    return text.empty() ? L"(none)" : text;
}

std::wstring RawGamepadBindingText(uint64_t buttons) {
    std::wstring text;
    for (int usage = 1; usage <= 64; ++usage) {
        uint64_t bit = 1ull << (usage - 1);
        if (buttons & bit) {
            if (!text.empty()) {
                text += L" + ";
            }
            text += L"Button " + std::to_wstring(usage);
        }
    }

    return text.empty() ? L"(none)" : text;
}

std::wstring BindingText(const InputBinding& binding) {
    if (binding.kind == BindingKind::Gamepad) {
        return L"Gamepad: " + GamepadBindingText(binding.gamepadButtons);
    }
    if (binding.kind == BindingKind::RawGamepad) {
        return L"HID Gamepad: " + RawGamepadBindingText(binding.rawGamepadButtons);
    }
    return KeyboardBindingText(binding);
}

int ClampDurationSeconds(int seconds) {
    return std::max(kMinDurationSeconds, std::min(kMaxDurationSeconds, seconds));
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void SaveConfig() {
    WritePrivateProfileStringW(L"target", L"path", g_targetPath.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"target", L"name", g_targetName.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"binding", L"type", std::to_wstring(static_cast<int>(g_binding.kind)).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"binding", L"modifiers", std::to_wstring(g_binding.modifiers).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"binding", L"vk", std::to_wstring(g_binding.vk).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"binding", L"gamepad_buttons", std::to_wstring(g_binding.gamepadButtons).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"binding", L"raw_gamepad_device", g_binding.rawGamepadDevice.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"binding", L"raw_gamepad_buttons", std::to_wstring(g_binding.rawGamepadButtons).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"behavior", L"duration_seconds", std::to_wstring(g_durationSeconds).c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"target", L"locked", g_targetLocked ? L"1" : L"0", g_configPath.c_str());
}

void LoadConfig() {
    g_configPath = GetConfigPath();

    wchar_t value[4096]{};
    GetPrivateProfileStringW(L"target", L"path", L"", value, static_cast<DWORD>(std::size(value)), g_configPath.c_str());
    g_targetPath = value;

    GetPrivateProfileStringW(L"target", L"name", L"", value, static_cast<DWORD>(std::size(value)), g_configPath.c_str());
    g_targetName = value;
    g_targetLocked = GetPrivateProfileIntW(L"target", L"locked", 1, g_configPath.c_str()) != 0;

    int bindingType = GetPrivateProfileIntW(L"binding", L"type", static_cast<int>(BindingKind::Keyboard), g_configPath.c_str());
    if (bindingType == static_cast<int>(BindingKind::Gamepad)) {
        g_binding.kind = BindingKind::Gamepad;
    } else if (bindingType == static_cast<int>(BindingKind::RawGamepad)) {
        g_binding.kind = BindingKind::RawGamepad;
    } else {
        g_binding.kind = BindingKind::Keyboard;
    }
    g_binding.modifiers = GetPrivateProfileIntW(L"binding", L"modifiers",
        GetPrivateProfileIntW(L"hotkey", L"modifiers", MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, g_configPath.c_str()),
        g_configPath.c_str());
    g_binding.vk = GetPrivateProfileIntW(L"binding", L"vk",
        GetPrivateProfileIntW(L"hotkey", L"vk", VK_F8, g_configPath.c_str()),
        g_configPath.c_str());
    g_binding.gamepadButtons = static_cast<WORD>(GetPrivateProfileIntW(L"binding", L"gamepad_buttons", XINPUT_GAMEPAD_A, g_configPath.c_str()));
    GetPrivateProfileStringW(L"binding", L"raw_gamepad_device", L"", value, static_cast<DWORD>(std::size(value)), g_configPath.c_str());
    g_binding.rawGamepadDevice = value;
    GetPrivateProfileStringW(L"binding", L"raw_gamepad_buttons", L"0", value, static_cast<DWORD>(std::size(value)), g_configPath.c_str());
    g_binding.rawGamepadButtons = _wcstoui64(value, nullptr, 10);
    g_binding.modifiers |= MOD_NOREPEAT;
    g_durationSeconds = ClampDurationSeconds(GetPrivateProfileIntW(L"behavior", L"duration_seconds", kDefaultDurationSeconds, g_configPath.c_str()));
}

bool IsOnlyModifier(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
        vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN;
}

bool RegisterConfiguredHotkey(HWND hwnd) {
    UnregisterHotKey(hwnd, kHotkeyId);
    if (g_binding.kind == BindingKind::Gamepad || g_binding.kind == BindingKind::RawGamepad) {
        return true;
    }
    return RegisterHotKey(hwnd, kHotkeyId, g_binding.modifiers, g_binding.vk) == TRUE;
}

int ReadDurationFromUi(HWND hwnd) {
    BOOL translated = FALSE;
    UINT value = GetDlgItemInt(hwnd, kEditDuration, &translated, FALSE);
    int seconds = translated ? static_cast<int>(value) : kDefaultDurationSeconds;
    g_durationSeconds = ClampDurationSeconds(seconds);

    if (g_durationEdit) {
        wchar_t current[32]{};
        GetWindowTextW(g_durationEdit, current, static_cast<int>(std::size(current)));
        std::wstring normalized = std::to_wstring(g_durationSeconds);
        if (normalized != current) {
            SetWindowTextW(g_durationEdit, normalized.c_str());
        }
    }

    SaveConfig();
    return g_durationSeconds;
}

void UpdateLabels() {
    std::wstring target = L"Target: ";
    if (g_targetPath.empty()) {
        target += L"(select a process)";
    } else {
        if (g_targetLocked) {
            target += L"[locked] ";
        }
        target += g_targetName.empty() ? L"(saved process)" : g_targetName;
        target += L"  ";
        target += g_targetPath;
    }
    SetText(g_targetLabel, target);

    std::wstring hotkey = g_capturingHotkey
        ? L"Binding: press a keyboard combo or a gamepad button..."
        : L"Binding: " + BindingText(g_binding);
    SetText(g_hotkeyLabel, hotkey);
}

void RefreshProcessList() {
    g_processes = EnumerateProcesses();

    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_processes.size(); ++i) {
        const auto& process = g_processes[i];

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(process.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        int row = ListView_InsertItem(g_list, &item);

        std::wstring pid = std::to_wstring(process.pid);
        ListView_SetItemText(g_list, row, 1, const_cast<LPWSTR>(pid.c_str()));
        ListView_SetItemText(g_list, row, 2, const_cast<LPWSTR>(process.path.c_str()));

        if (!g_targetPath.empty() && _wcsicmp(process.path.c_str(), g_targetPath.c_str()) == 0) {
            ListView_SetItemState(g_list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, row, FALSE);
        }
    }
}

bool TrySelectCurrentProcessTarget(bool quiet) {
    int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (row < 0) {
        if (!quiet) {
            MessageBoxW(nullptr, L"Select a process first.", L"Deconnector", MB_ICONINFORMATION);
        }
        return false;
    }

    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(g_list, &item)) {
        return false;
    }

    size_t index = static_cast<size_t>(item.lParam);
    if (index >= g_processes.size()) {
        return false;
    }

    g_targetPath = g_processes[index].path;
    g_targetName = g_processes[index].name;
    SaveConfig();
    UpdateLabels();
    return true;
}

void SelectCurrentProcessTarget() {
    TrySelectCurrentProcessTarget(false);
}

void SetTargetLock(HWND hwnd, bool locked) {
    g_targetLocked = locked;
    if (g_lockTargetCheck) {
        SendMessageW(g_lockTargetCheck, BM_SETCHECK, g_targetLocked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (!g_targetLocked || !g_targetPath.empty()) {
        SaveConfig();
    }
    UpdateLabels();

    if (g_targetLocked) {
        SetTimer(hwnd, kProcessRefreshTimer, 2000, nullptr);
        RefreshProcessList();
    } else {
        KillTimer(hwnd, kProcessRefreshTimer);
    }
}

class WfpBlocker {
public:
    WfpBlocker() = default;
    WfpBlocker(const WfpBlocker&) = delete;
    WfpBlocker& operator=(const WfpBlocker&) = delete;

    ~WfpBlocker() {
        Stop();
    }

    DWORD Start(const std::wstring& appPath) {
        Stop();

        FWPM_SESSION0 session{};
        session.displayData.name = const_cast<wchar_t*>(L"Deconnector dynamic session");
        session.flags = FWPM_SESSION_FLAG_DYNAMIC;

        DWORD error = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine_);
        if (error != ERROR_SUCCESS) {
            return error;
        }

        error = FwpmGetAppIdFromFileName0(appPath.c_str(), &appId_);
        if (error != ERROR_SUCCESS) {
            Stop();
            return error;
        }

        FWPM_SUBLAYER0 sublayer{};
        sublayer.subLayerKey = kSublayerKey;
        sublayer.displayData.name = const_cast<wchar_t*>(L"Deconnector temporary blocks");
        sublayer.displayData.description = const_cast<wchar_t*>(L"Temporary WFP filters created by Deconnector");
        sublayer.weight = 0x100;

        error = FwpmSubLayerAdd0(engine_, &sublayer, nullptr);
        if (error != ERROR_SUCCESS && error != FWP_E_ALREADY_EXISTS) {
            Stop();
            return error;
        }

        const GUID layers[] = {
            FWPM_LAYER_ALE_AUTH_CONNECT_V4,
            FWPM_LAYER_ALE_AUTH_CONNECT_V6,
            FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
            FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6
        };

        DWORD lastError = ERROR_SUCCESS;
        for (const GUID& layer : layers) {
            DWORD addError = AddFilter(layer);
            if (addError != ERROR_SUCCESS) {
                lastError = addError;
            }
        }

        if (filterIds_.empty()) {
            Stop();
            return lastError == ERROR_SUCCESS ? ERROR_CAN_NOT_COMPLETE : lastError;
        }

        return ERROR_SUCCESS;
    }

    void Stop() {
        if (engine_) {
            for (UINT64 id : filterIds_) {
                FwpmFilterDeleteById0(engine_, id);
            }
            filterIds_.clear();
        }

        if (appId_) {
            FwpmFreeMemory0(reinterpret_cast<void**>(&appId_));
            appId_ = nullptr;
        }

        if (engine_) {
            FwpmEngineClose0(engine_);
            engine_ = nullptr;
        }
    }

private:
    DWORD AddFilter(const GUID& layer) {
        FWPM_FILTER_CONDITION0 condition{};
        condition.fieldKey = FWPM_CONDITION_ALE_APP_ID;
        condition.matchType = FWP_MATCH_EQUAL;
        condition.conditionValue.type = FWP_BYTE_BLOB_TYPE;
        condition.conditionValue.byteBlob = appId_;

        FWPM_FILTER0 filter{};
        filter.displayData.name = const_cast<wchar_t*>(L"Deconnector temporary process block");
        filter.displayData.description = const_cast<wchar_t*>(L"Blocks one executable path for a short interval");
        filter.layerKey = layer;
        filter.subLayerKey = kSublayerKey;
        filter.action.type = FWP_ACTION_BLOCK;
        filter.weight.type = FWP_EMPTY;
        filter.numFilterConditions = 1;
        filter.filterCondition = &condition;

        UINT64 filterId = 0;
        DWORD error = FwpmFilterAdd0(engine_, &filter, nullptr, &filterId);
        if (error == ERROR_SUCCESS) {
            filterIds_.push_back(filterId);
        }
        return error;
    }

    HANDLE engine_ = nullptr;
    FWP_BYTE_BLOB* appId_ = nullptr;
    std::vector<UINT64> filterIds_;
};

void StartBlock(HWND hwnd) {
    if (g_targetPath.empty()) {
        MessageBoxW(hwnd, L"Select a process before disconnecting.", L"Deconnector", MB_ICONINFORMATION);
        return;
    }
    if (g_blockActive.exchange(true)) {
        return;
    }

    int durationSeconds = ReadDurationFromUi(hwnd);
    g_blockEndsAt = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
    SetTimer(hwnd, kCountdownTimer, 100, nullptr);
    EnableWindow(g_disconnectButton, FALSE);
    SetText(g_statusLabel, L"Status: activating block...");

    std::wstring path = g_targetPath;
    std::thread([hwnd, path, durationSeconds]() {
        WfpBlocker blocker;
        DWORD error = blocker.Start(path);
        if (error != ERROR_SUCCESS) {
            auto* errorMessage = new std::wstring(FormatWin32Error(error));
            g_blockActive = false;
            PostMessageW(hwnd, kBlockFailedMessage, error, reinterpret_cast<LPARAM>(errorMessage));
            return;
        }

        std::this_thread::sleep_for(std::chrono::seconds(durationSeconds));
        blocker.Stop();
        g_blockActive = false;
        PostMessageW(hwnd, kBlockDoneMessage, 0, 0);
    }).detach();
}

void UpdateCountdown() {
    if (!g_blockActive) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(g_blockEndsAt - now).count();
    if (remaining < 0) {
        remaining = 0;
    }

    wchar_t text[128]{};
    swprintf_s(text, L"Status: disconnected, %.1f s remaining", remaining / 1000.0);
    SetText(g_statusLabel, text);
}

WORD ReadGamepadButtons(DWORD controllerIndex) {
    XINPUT_STATE state{};
    if (XInputGetState(controllerIndex, &state) != ERROR_SUCCESS) {
        return 0;
    }
    return state.Gamepad.wButtons;
}

void PrimeGamepadStates() {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        g_previousGamepadButtons[i] = ReadGamepadButtons(i);
    }
}

void CaptureGamepadBinding(HWND hwnd, WORD buttons) {
    g_binding.kind = BindingKind::Gamepad;
    g_binding.gamepadButtons = buttons;
    g_capturingHotkey = false;
    RegisterConfiguredHotkey(hwnd);
    SaveConfig();
    UpdateLabels();
}

void PollGamepads(HWND hwnd) {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        WORD current = ReadGamepadButtons(i);
        WORD previous = g_previousGamepadButtons[i];
        WORD newlyPressed = current & ~previous;

        if (g_capturingHotkey && current != 0 && newlyPressed != 0) {
            CaptureGamepadBinding(hwnd, current);
            g_previousGamepadButtons[i] = current;
            return;
        }

        if (!g_capturingHotkey &&
            g_binding.kind == BindingKind::Gamepad &&
            g_binding.gamepadButtons != 0 &&
            (current & g_binding.gamepadButtons) == g_binding.gamepadButtons &&
            (previous & g_binding.gamepadButtons) != g_binding.gamepadButtons) {
            StartBlock(hwnd);
        }

        g_previousGamepadButtons[i] = current;
    }
}

bool RegisterRawGamepadInput(HWND hwnd) {
    RAWINPUTDEVICE devices[2]{};

    devices[0].usUsagePage = 0x01;
    devices[0].usUsage = 0x05; // Game Pad
    devices[0].dwFlags = RIDEV_INPUTSINK;
    devices[0].hwndTarget = hwnd;

    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x04; // Joystick
    devices[1].dwFlags = RIDEV_INPUTSINK;
    devices[1].hwndTarget = hwnd;

    return RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)), sizeof(RAWINPUTDEVICE)) == TRUE;
}

std::wstring RawDeviceName(HANDLE device) {
    UINT size = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0) {
        return L"device:" + std::to_wstring(reinterpret_cast<uintptr_t>(device));
    }

    std::vector<wchar_t> name(size + 1);
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) == static_cast<UINT>(-1)) {
        return L"device:" + std::to_wstring(reinterpret_cast<uintptr_t>(device));
    }

    return name.data();
}

bool ExtractRawGamepadButtons(const RAWINPUT* raw, uint64_t& buttons) {
    buttons = 0;
    if (!raw || raw->header.dwType != RIM_TYPEHID) {
        return false;
    }

    UINT preparsedSize = 0;
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_PREPARSEDDATA, nullptr, &preparsedSize) != 0 || preparsedSize == 0) {
        return false;
    }

    std::vector<BYTE> preparsedBuffer(preparsedSize);
    if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_PREPARSEDDATA, preparsedBuffer.data(), &preparsedSize) == static_cast<UINT>(-1)) {
        return false;
    }

    auto preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsedBuffer.data());
    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS || caps.NumberInputButtonCaps == 0) {
        return false;
    }

    USHORT buttonCapCount = caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> buttonCaps(buttonCapCount);
    if (HidP_GetButtonCaps(HidP_Input, buttonCaps.data(), &buttonCapCount, preparsed) != HIDP_STATUS_SUCCESS) {
        return false;
    }

    constexpr USAGE kButtonUsagePage = 0x09;
    const BYTE* reportData = raw->data.hid.bRawData;

    for (DWORD reportIndex = 0; reportIndex < raw->data.hid.dwCount; ++reportIndex) {
        auto report = reinterpret_cast<PCHAR>(const_cast<BYTE*>(reportData + reportIndex * raw->data.hid.dwSizeHid));

        for (USHORT i = 0; i < buttonCapCount; ++i) {
            if (buttonCaps[i].UsagePage != kButtonUsagePage) {
                continue;
            }

            ULONG usageLength = HidP_MaxUsageListLength(HidP_Input, buttonCaps[i].UsagePage, preparsed);
            if (usageLength == 0) {
                continue;
            }

            std::vector<USAGE> usages(usageLength);
            NTSTATUS status = HidP_GetUsages(
                HidP_Input,
                buttonCaps[i].UsagePage,
                buttonCaps[i].LinkCollection,
                usages.data(),
                &usageLength,
                preparsed,
                report,
                raw->data.hid.dwSizeHid);

            if (status != HIDP_STATUS_SUCCESS) {
                continue;
            }

            for (ULONG usageIndex = 0; usageIndex < usageLength; ++usageIndex) {
                USAGE usage = usages[usageIndex];
                if (usage >= 1 && usage <= 64) {
                    buttons |= 1ull << (usage - 1);
                }
            }
        }
    }

    return true;
}

void CaptureRawGamepadBinding(HWND hwnd, const std::wstring& deviceName, uint64_t buttons) {
    g_binding.kind = BindingKind::RawGamepad;
    g_binding.rawGamepadDevice = deviceName;
    g_binding.rawGamepadButtons = buttons;
    g_capturingHotkey = false;
    RegisterConfiguredHotkey(hwnd);
    SaveConfig();
    UpdateLabels();
}

void HandleRawInput(HWND hwnd, LPARAM lParam) {
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }

    auto raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
    uint64_t buttons = 0;
    if (!ExtractRawGamepadButtons(raw, buttons)) {
        return;
    }

    std::wstring deviceName = RawDeviceName(raw->header.hDevice);
    uint64_t previous = g_previousRawGamepadButtons[deviceName];
    uint64_t newlyPressed = buttons & ~previous;

    if (g_capturingHotkey && buttons != 0 && newlyPressed != 0) {
        CaptureRawGamepadBinding(hwnd, deviceName, buttons);
        g_previousRawGamepadButtons[deviceName] = buttons;
        return;
    }

    if (!g_capturingHotkey &&
        g_binding.kind == BindingKind::RawGamepad &&
        g_binding.rawGamepadButtons != 0 &&
        deviceName == g_binding.rawGamepadDevice &&
        (buttons & g_binding.rawGamepadButtons) == g_binding.rawGamepadButtons &&
        (previous & g_binding.rawGamepadButtons) != g_binding.rawGamepadButtons) {
        StartBlock(hwnd);
    }

    g_previousRawGamepadButtons[deviceName] = buttons;
}

void AddColumn(HWND list, int index, int width, const wchar_t* text) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.iSubItem = index;
    column.cx = width;
    column.pszText = const_cast<LPWSTR>(text);
    ListView_InsertColumn(list, index, &column);
}

void ResizeControls(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);

    int padding = 12;
    int buttonWidth = 132;
    int buttonHeight = 30;
    int labelHeight = 24;
    int bottomHeight = 104;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    MoveWindow(g_list, padding, padding, width - padding * 2, height - bottomHeight - padding, TRUE);

    int y = height - bottomHeight + 4;
    MoveWindow(g_refreshButton, padding, y, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_bindButton, padding + buttonWidth + 8, y, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_disconnectButton, padding + (buttonWidth + 8) * 2, y, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_durationLabel, padding + (buttonWidth + 8) * 3 + 12, y + 6, 88, 20, TRUE);
    MoveWindow(g_durationEdit, padding + (buttonWidth + 8) * 3 + 102, y + 2, 76, 24, TRUE);
    MoveWindow(g_durationSpin, padding + (buttonWidth + 8) * 3 + 102, y + 2, 76, 24, TRUE);
    MoveWindow(g_lockTargetCheck, padding + (buttonWidth + 8) * 3 + 194, y + 4, 124, 24, TRUE);

    y += buttonHeight + 8;
    MoveWindow(g_targetLabel, padding, y, width - padding * 2, labelHeight, TRUE);
    y += labelHeight;
    MoveWindow(g_hotkeyLabel, padding, y, width / 2 - padding, labelHeight, TRUE);
    MoveWindow(g_statusLabel, width / 2, y, width / 2 - padding, labelHeight, TRUE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0,
            0,
            0,
            0,
            hwnd,
            ControlId(kListProcesses),
            nullptr,
            nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        AddColumn(g_list, 0, 180, L"Process");
        AddColumn(g_list, 1, 80, L"PID");
        AddColumn(g_list, 2, 620, L"Path");

        g_refreshButton = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, ControlId(kButtonRefresh), nullptr, nullptr);
        g_bindButton = CreateWindowW(L"BUTTON", L"Bind Hotkey", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, ControlId(kButtonBindHotkey), nullptr, nullptr);
        g_disconnectButton = CreateWindowW(L"BUTTON", L"Disconnect", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, ControlId(kButtonDisconnect), nullptr, nullptr);
        g_durationLabel = CreateWindowW(L"STATIC", L"Seconds:", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, hwnd, ControlId(kStaticDuration), nullptr, nullptr);
        g_durationEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, ControlId(kEditDuration), nullptr, nullptr);
        g_durationSpin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0, hwnd, ControlId(kSpinDuration), nullptr, nullptr);
        SendMessageW(g_durationSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(g_durationEdit), 0);
        SendMessageW(g_durationSpin, UDM_SETRANGE32, kMinDurationSeconds, kMaxDurationSeconds);
        SendMessageW(g_durationSpin, UDM_SETPOS32, 0, g_durationSeconds);
        g_lockTargetCheck = CreateWindowW(L"BUTTON", L"Lock target", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, ControlId(kCheckLockTarget), nullptr, nullptr);
        SendMessageW(g_lockTargetCheck, BM_SETCHECK, g_targetLocked ? BST_CHECKED : BST_UNCHECKED, 0);
        g_targetLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, hwnd, ControlId(kStaticTarget), nullptr, nullptr);
        g_hotkeyLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, hwnd, ControlId(kStaticHotkey), nullptr, nullptr);
        g_statusLabel = CreateWindowW(L"STATIC", L"Status: idle", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, hwnd, ControlId(kStaticStatus), nullptr, nullptr);

        RegisterConfiguredHotkey(hwnd);
        if (!RegisterRawGamepadInput(hwnd)) {
            SetText(g_statusLabel, L"Status: raw gamepad input unavailable");
        }
        PrimeGamepadStates();
        SetTimer(hwnd, kGamepadPollTimer, 50, nullptr);
        if (g_targetLocked) {
            SetTimer(hwnd, kProcessRefreshTimer, 2000, nullptr);
        }
        UpdateLabels();
        RefreshProcessList();
        ResizeControls(hwnd);
        return 0;
    }
    case WM_SIZE:
        ResizeControls(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kButtonRefresh:
            RefreshProcessList();
            return 0;
        case kButtonBindHotkey:
            g_capturingHotkey = true;
            PrimeGamepadStates();
            SetFocus(hwnd);
            UpdateLabels();
            return 0;
        case kButtonDisconnect:
            if (!g_targetLocked || g_targetPath.empty()) {
                TrySelectCurrentProcessTarget(false);
            }
            StartBlock(hwnd);
            return 0;
        case kEditDuration:
            if (HIWORD(wParam) == EN_CHANGE && g_durationEdit != nullptr) {
                BOOL translated = FALSE;
                UINT value = GetDlgItemInt(hwnd, kEditDuration, &translated, FALSE);
                if (translated) {
                    g_durationSeconds = ClampDurationSeconds(static_cast<int>(value));
                    SaveConfig();
                }
            }
            return 0;
        case kCheckLockTarget:
            SetTargetLock(hwnd, SendMessageW(g_lockTargetCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        default:
            break;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == kListProcesses &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
            auto* item = reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((item->uChanged & LVIF_STATE) && (item->uNewState & LVIS_SELECTED)) {
                if (!g_targetLocked || g_targetPath.empty()) {
                    SelectCurrentProcessTarget();
                }
            }
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capturingHotkey && !IsOnlyModifier(static_cast<UINT>(wParam))) {
            UINT modifiers = MOD_NOREPEAT;
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                modifiers |= MOD_CONTROL;
            }
            if (GetKeyState(VK_MENU) & 0x8000) {
                modifiers |= MOD_ALT;
            }
            if (GetKeyState(VK_SHIFT) & 0x8000) {
                modifiers |= MOD_SHIFT;
            }
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) {
                modifiers |= MOD_WIN;
            }

            if ((modifiers & ~(MOD_NOREPEAT)) == 0) {
                MessageBoxW(hwnd, L"Use at least one modifier key, such as Ctrl or Alt.", L"Deconnector", MB_ICONINFORMATION);
                return 0;
            }

            g_binding.kind = BindingKind::Keyboard;
            g_binding.modifiers = modifiers;
            g_binding.vk = static_cast<UINT>(wParam);
            g_capturingHotkey = false;

            if (!RegisterConfiguredHotkey(hwnd)) {
                MessageBoxW(hwnd, L"That hotkey is already in use by another application.", L"Deconnector", MB_ICONWARNING);
            } else {
                SaveConfig();
            }
            UpdateLabels();
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            StartBlock(hwnd);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kCountdownTimer) {
            UpdateCountdown();
            return 0;
        }
        if (wParam == kGamepadPollTimer) {
            PollGamepads(hwnd);
            return 0;
        }
        if (wParam == kProcessRefreshTimer) {
            if (g_targetLocked) {
                RefreshProcessList();
            }
            return 0;
        }
        break;
    case WM_INPUT:
        HandleRawInput(hwnd, lParam);
        return 0;
    case kBlockDoneMessage:
        KillTimer(hwnd, kCountdownTimer);
        EnableWindow(g_disconnectButton, TRUE);
        SetText(g_statusLabel, L"Status: restored");
        return 0;
    case kBlockFailedMessage: {
        KillTimer(hwnd, kCountdownTimer);
        EnableWindow(g_disconnectButton, TRUE);
        auto* detail = reinterpret_cast<std::wstring*>(lParam);
        std::wstring dialogMessage = L"Failed to create WFP filters.";
        if (detail) {
            dialogMessage += L"\n\n";
            dialogMessage += *detail;
            delete detail;
        }
        SetText(g_statusLabel, L"Status: failed");
        MessageBoxW(hwnd, dialogMessage.c_str(), L"Deconnector", MB_ICONERROR);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kCountdownTimer);
        KillTimer(hwnd, kGamepadPollTimer);
        KillTimer(hwnd, kProcessRefreshTimer);
        UnregisterHotKey(hwnd, kHotkeyId);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&controls);

    LoadConfig();

    const wchar_t* className = L"DeconnectorMainWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        className,
        L"Deconnector",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        620,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
