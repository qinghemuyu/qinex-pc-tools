// 表单草稿独立于运行配置；跨屏重建时保留输入。
enum {
    ID_HOME_STATE = 200, ID_HOME_DEVICE, ID_HOME_PREVIEW, ID_HOME_DETAIL,
    ID_HOME_PRIMARY, ID_HOME_CHANGE_DEVICE, ID_HOME_CHANGE_PREVIEW, ID_HOME_APPEARANCE, ID_HOME_CALIBRATE,
    ID_NEXT, ID_BACK, ID_SAVE_STATE, ID_PAGE_TITLE,
    ID_LAN_ADDRESS, ID_COPY_ADDRESS, ID_BIND_SOURCE, ID_NETWORK_ADVANCED,
    ID_DEVICE_HELP, ID_DEVICE_STATE, ID_SOURCE_LABEL, ID_DIAGNOSTICS,
    ID_MAPPING_STYLE, ID_ALIGNMENT_CHECK, ID_RESUME_LAUNCH,
    ID_SAVE_SCENE, ID_LOAD_SCENE, ID_HOME_REGION, ID_CHECK_ALIGNMENT,
    ID_CAPTURE_DEVICE, ID_CAPTURE_REFRESH, ID_CAPTURE_MONITOR, ID_CAPTURE_TOGGLE, ID_CAPTURE_STOP, ID_CAPTURE_STATE,
    ID_CAPTURE_SPEC, ID_CAPTURE_EXACT_MODE, ID_CAPTURE_ADVANCED, ID_CAPTURE_PRESENT_MODE, ID_CAPTURE_METRICS,
    ID_CAPTURE_DIAGNOSTICS, ID_MAPPING_HELP, ID_CAPTURE_RANGE,
    ID_FUSION_STATE, ID_FUSION_MONITOR, ID_FUSION_COLOR, ID_FUSION_PICK,
    ID_FUSION_GREEN, ID_FUSION_BLUE, ID_FUSION_BLACK, ID_FUSION_TOGGLE,
    ID_FUSION_PRESENT_MODE, ID_FUSION_RANGE
};
enum UiCondition {
    UiP4, UiNetworkExtra, UiSourceAddress, UiMonitor, UiWindow, UiCustom,
    UiGeometryExtra, UiGeometrySimple, UiGeometryFrameInput, UiCaptureGeometryFrame,
    UiCapture, UiCaptureSpecBasic, UiCaptureSpecAdvanced, UiCaptureGeometrySimple,
    UiFusion, UiMapping, UiDesktopTarget
};
struct UiConditional { HWND hwnd{}; UiCondition kind{}; };
struct UiValue {
    std::wstring text;
    std::wstring stableIdentity;
    int selected = -1, checked = -1;
    UINT_PTR identity = 0;
    bool operator==(const UiValue& other) const {
        return text == other.text && stableIdentity == other.stableIdentity && checked == other.checked && identity == other.identity;
    }
};
struct LocalAddress { std::wstring address, label; };
std::vector<UiConditional> uiConditional;
std::vector<LocalAddress> localAddresses;
std::vector<std::wstring> uiCaptureDeviceIds;
std::vector<std::wstring> uiCaptureDeviceNames;
std::vector<std::wstring> uiCaptureDeviceLabels;
std::vector<bool> uiCaptureDeviceAvailable;
std::vector<std::wstring> uiCapturePresetModeKeys, uiCaptureExactModeKeys;
std::vector<int> uiCapturePresetFps;
std::vector<bool> uiCapturePresetAvailable, uiCaptureExactAvailable;
std::map<int, UiValue> uiDraft, uiBaseline;
bool uiBuilding = false, uiSkipRemember = false;
bool uiCaptureDefaultsApplied = false;
int uiDisplayedSource = 0;
std::wstring uiSourceAddresses[2]{L"127.0.0.1", L""};
HFONT headingFont{};
void InventoryAddresses();
void RefreshCaptureModesForDevice(const std::wstring& deviceId);
void TrackControls(size_t first, UiCondition condition);
void UpdateVisibility();
void UpdatePresentation();
bool ReadPage(Settings& candidate, int scope, bool showErrors = true);
std::map<int, UiValue> CaptureUiValues() const;
void RememberDraft();
void RestoreDraft();
void ClearDraft();
void ReloadControls();
void ReloadPage(int scope);
void CommitDraft(int scope = -1);
bool HasDraft(int scope = -1) const;
bool HandleUiCommand(int id, int event);
void NextStep();
void CopyComputerAddress();
void ToggleCaptureOutput();
