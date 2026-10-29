enum {
    ID_ANDROID_DEVICE = 800, ID_ANDROID_REFRESH, ID_ANDROID_WIRELESS, ID_ANDROID_INSPECT,
    ID_ANDROID_PHONE, ID_ANDROID_ROOT, ID_ANDROID_USE_ROOT, ID_ANDROID_ENABLE,
    ID_ANDROID_RESTORE_SETTING, ID_ANDROID_REBOOT, ID_ANDROID_SETTING,
    ID_ANDROID_PREVIEW, ID_ANDROID_SHAPE, ID_ANDROID_EDIT, ID_ANDROID_PC_IMAGE,
    ID_ANDROID_SIZE, ID_ANDROID_PROFILE, ID_ANDROID_VISIBLE, ID_ANDROID_BUILD,
    ID_ANDROID_EXPORT, ID_ANDROID_INSTALL, ID_ANDROID_RESTORE_IMAGE,
    ID_ANDROID_ANALYZE, ID_ANDROID_VERIFY, ID_ANDROID_PROGRESS, ID_ANDROID_CANCEL,
    ID_ANDROID_DETAILS, ID_ANDROID_POLICY, ID_ANDROID_TOUCH_STATE,
    ID_ANDROID_TOUCH_ENABLE, ID_ANDROID_TOUCH_DISABLE, ID_ANDROID_TOUCH_RESTORE,
    ID_ANDROID_TOUCH_NOTE
};
qp::android::ToolsService androidTools;
bool androidInitialized = false;
bool androidDevicesNeedRefresh = true;
std::string androidSerial, androidEndpoint;
std::wstring androidImagePath, androidDetails;
int androidShape = 0, androidSizeDp = 32, androidProfile = 0;
double androidHotspotX = .5, androidHotspotY = .5;
bool androidVisible = false, androidUseRoot = false;
uint64_t androidCompletedGeneration = 0;
std::shared_ptr<const qp::CustomCursorImage> androidPreview;
std::vector<qp::android::Device> androidDevices;
void BuildAndroidTools();
void RememberAndroidDraft();
void UpdateAndroidTools();
bool HandleAndroidCommand(int id, int event);
void BeginAndroidTask(qp::android::Action action, const std::wstring& exportPath = {});
void PaintAndroidPreview(const DRAWITEMSTRUCT& item);
bool SelectAndroidImage();
void SaveAndroidPreferences();
