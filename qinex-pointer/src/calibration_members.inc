// 校准操作只在 UI 线程运行；每次截图同时冻结输入坐标和显示几何。
enum {
    ID_CAL_LIST = 300, ID_CAL_UNDO, ID_CAL_DETAIL, ID_CAL_CANCEL, ID_CAL_INDEPENDENT,
    ID_CAL_MORE, ID_CAL_RANGE, ID_CAL_GRID_FIRST = 310, ID_CAL_GRID_LAST = 318
};
enum class CapturePhase { Idle, Preparing, Waiting, Stable, Hiding, Marking };
struct CalibrationUndo {
    std::vector<Sample> samples;
    std::vector<int> slots;
    std::vector<uint64_t> tokens, fittedTokens;
    qp::Affine calibration;
    size_t fitCount = 0;
    uint64_t fitToken = 0;
    int slot = 0;
    std::wstring result;
};
struct CalibrationSession {
    CalibrationUndo current;
    std::vector<CalibrationUndo> undo;
};
CapturePhase calibrationPhase = CapturePhase::Idle;
HWND calibrationPrompt{};
uint64_t calibrationDeadline = 0, lastCalibrationDownMs = 0;
uint64_t calibrationPressedSince = 0;
uint64_t calibrationCaptureFrameBaseline = 0, calibrationCaptureStableSince = 0;
uint64_t nextCalibrationToken = 1, calibrationFitToken = 0;
int calibrationSlot = 0, snapshotSlot = 0, magnifierZoom = 4;
std::vector<int> calibrationSlots;
std::vector<uint64_t> calibrationTokens, fittedCalibrationTokens;
std::vector<double> calibrationErrors;
std::vector<CalibrationUndo> calibrationUndo;
std::map<std::wstring, CalibrationSession> calibrationSessions;
std::wstring calibrationResult, calibrationPromptTitle, calibrationPromptBody;
bool calibrationDetails = false;
int calibrationLinkControl = 0;
std::optional<qp::Point> magnifierPoint;
void CalibrationControls(int page = 3);
void TickCalibration(uint64_t now);
void CancelCalibration(bool restoreMain = true);
bool HandleCalibrationCommand(int id);
void ObserveCalibrationPacket(const qp::Packet& packet, uint64_t receivedMs);
void ShowCalibrationPrompt();
void UpdateCalibrationPrompt(const std::wstring& title, const std::wstring& body);
void PaintCalibrationPrompt(HDC dc);
void SyncCalibrationSamples();
void PushCalibrationUndo();
void UndoCalibration();
void ClearCalibrationSession();
void ResetImportedCalibrationSession();
void SwitchCalibrationSource(const Settings& previous, const Settings& next);
void MoveSampleMark(int dx, int dy);
bool CalibrationFresh(uint64_t now) const;
