#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/menu.h>
#include <gui/dialogs/dialog_box.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <utils/wav.h>
#include <dsp/sink/handler_sink.h>
#include <volk/volk.h>
#include <libhackrf/hackrf.h>

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>

#ifdef _WIN32
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hackrf_tx",
    /* Description:     */ "HackRF transmit module (IQ record & replay)",
    /* Author:          */ "SDR++",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

/*
 * Record and replay for HackRF One.
 *
 * Record: a private VFO (bandwidth == sample rate) is created and its complex
 * baseband output is written to a 2-channel float32 WAV, which is the same
 * format the built-in file source and IQ exporter understand.
 *
 * Transmit: the WAV is preloaded into an int8 buffer (HackRF native format,
 * interleaved I/Q) and pushed to the device from the libhackrf TX callback.
 * HackRF is half duplex, so the receiver is stopped for the duration of the
 * transmission and restarted afterwards.
 */

static const int SAMPLE_RATES[] = { 2000000, 4000000, 5000000, 8000000, 10000000, 16000000, 20000000 };
static const char* SAMPLE_RATES_TXT = "2 MHz\0" "4 MHz\0" "5 MHz\0" "8 MHz\0" "10 MHz\0" "16 MHz\0" "20 MHz\0";
#define SR_COUNT ((int)(sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0])))

// Recording depth. Int16 halves the file size compared to float32 and is still
// read by SDR++'s own file source; float32 keeps full precision.
static const char* REC_DEPTH_TXT = "Int16\0" "Float32\0";
#define REC_DEPTH_INT16   0
#define REC_DEPTH_FLOAT32 1

// What to record. The waterfall modes follow the current view automatically, so
// the recorded slice always matches what the operator is looking at.
enum RecSource {
    REC_SRC_WATERFALL = 0,  // whole source bandwidth, centred on the waterfall
    REC_SRC_RADIO     = 1,  // follow the selected radio (VFO) in the waterfall
    REC_SRC_MANUAL    = 2   // explicit sample rate, centred on the waterfall
};
static const char* REC_SOURCE_TXT = "Waterfall (auto)\0" "Active radio (auto)\0" "Manual\0";

// libhackrf accepts roughly 2-20 MS/s for both directions. A recording outside
// that window can be made but not transmitted, so the recorder clamps to it.
#define HACKRF_MIN_TX_SR 2000000
#define HACKRF_MAX_TX_SR 20000000

// Upper bound for the preloaded transmit buffer (int8, 2 bytes per complex sample)
#define MAX_TX_BYTES (1024ull * 1024ull * 1024ull)

enum TxState {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_LOADING,
    STATE_TRANSMITTING
};

struct WavInfo {
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;
};

class HackRFTxModule : public ModuleManager::Instance {
public:
    HackRFTxModule(std::string name) {
        this->name = name;
        // Build the path with the platform's own separator. Mixing '/' into a
        // Windows path makes Explorer resolve it to the wrong folder.
        this->recDir = (std::filesystem::path((std::string)core::args["root"]) / "recordings")
                           .make_preferred()
                           .string();

        // The source module also calls hackrf_init(). libusb reference counts its
        // own initialisation, so a second call is harmless, but we deliberately
        // never call hackrf_exit() to avoid tearing down state that another
        // module instance still relies on.
        hackrf_init();

        // Defaults
        sampleRateId = 0;
        sampleRate = SAMPLE_RATES[sampleRateId];
        txVga = 20;
        amp = false;
        txFreq = 0.0;

        // Load config
        config.acquire();
        if (config.conf.contains("sampleRate")) {
            int sr = config.conf["sampleRate"];
            for (int i = 0; i < SR_COUNT; i++) {
                if (SAMPLE_RATES[i] == sr) { sampleRateId = i; sampleRate = sr; }
            }
        }
        if (config.conf.contains("txVgaGain")) { txVga = std::clamp<int>(config.conf["txVgaGain"], 0, 47); }
        if (config.conf.contains("recDepth")) { recDepthId = std::clamp<int>(config.conf["recDepth"], 0, 1); }
        if (config.conf.contains("recSource")) {
            recSourceId = std::clamp<int>(config.conf["recSource"], 0, 2);
            recSource = (RecSource)recSourceId;
        }
        if (config.conf.contains("amp")) { amp = config.conf["amp"]; }
        if (config.conf.contains("txFreq")) { txFreq = config.conf["txFreq"]; }
        if (config.conf.contains("lastFile")) { lastFile = config.conf["lastFile"].get<std::string>(); }
        config.release();

        // Remove leftover recording VFOs from a previous session
        refreshFiles();

        gui::menu.registerEntry(displayName, menuHandler, this, this);
    }

    ~HackRFTxModule() {
        gui::menu.removeEntry(displayName);

        stopTransmit(true);
        stopRecord();

        if (txThread.joinable()) { txThread.join(); }
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        stopTransmit(true);
        stopRecord();
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    // ---------------------------------------------------------------- recording

    void startRecord() {
        if (state != STATE_IDLE) { return; }

        if (!gui::mainWindow.sdrIsRunning()) {
            showError("The receiver must be running before recording IQ.");
            return;
        }

        // Resolve the slice to record from the current view
        double srcSr = sigpath::iqFrontEnd.getSampleRate();
        if (srcSr <= 0.0) {
            showError("Could not determine the source sample rate.");
            return;
        }

        double sr = 0.0;
        double offset = 0.0;

        if (recSource == REC_SRC_RADIO) {
            std::string vfoName = gui::waterfall.selectedVFO;
            if (vfoName.empty() || !sigpath::vfoManager.vfoExists(vfoName)) {
                showError("No radio is selected.\nSelect a VFO in the waterfall first.");
                return;
            }
            sr = sigpath::vfoManager.getBandwidth(vfoName);
            offset = sigpath::vfoManager.getOffset(vfoName);
        }
        else if (recSource == REC_SRC_MANUAL) {
            sr = sampleRate;
        }
        else {
            sr = srcSr;
        }

        sr = clampToTxRange(sr, srcSr);

        // The VFO is owned by this module: SDR++ gives each VFO a single consumer,
        // and no other module touches a VFO it did not create itself.
        recVfo = sigpath::vfoManager.createVFO(name + "_txrec", ImGui::WaterfallVFO::REF_CENTER, offset,
                                               sr, sr, sr, sr, true);
        if (!recVfo) {
            showError("Could not create the recording VFO.");
            return;
        }

        // Absolute centre of the recorded slice
        recFreq = gui::waterfall.getCenterFrequency() + offset;
        recSampleRate = (int)sr;
        recSourceSr = srcSr;
        recSamples = 0;

        std::error_code ec;
        std::filesystem::create_directories(recDir, ec);

        recPath = (std::filesystem::path(recDir) / ("rec_" + timestamp() + ".wav")).string();

        recWriter.setChannels(2);
        recWriter.setSamplerate(recSampleRate);
        recWriter.setFormat(wav::FORMAT_WAV);
        recWriter.setSampleType(recDepthId == REC_DEPTH_INT16 ? wav::SAMP_TYPE_INT16 : wav::SAMP_TYPE_FLOAT32);

        if (!recWriter.open(recPath)) {
            sigpath::vfoManager.deleteVFO(recVfo);
            recVfo = NULL;
            showError("Could not create the recording file:\n" + recPath);
            return;
        }

        recHandler.init(recVfo->output, recordHandler, this);
        recHandler.start();

        state = STATE_RECORDING;
        statusStr = "Recording";
        flog::info("HackRFTx: recording to {0} ({1} S/s, {2} Hz)", recPath, recSampleRate, recFreq);
    }

    void stopRecord() {
        if (state != STATE_RECORDING) { return; }

        recHandler.stop();

        if (recVfo) {
            sigpath::vfoManager.deleteVFO(recVfo);
            recVfo = NULL;
        }

        recWriter.close();

        // Drop empty recordings so the file list stays clean
        if (recSamples == 0) {
            std::error_code ec;
            std::filesystem::remove(recPath, ec);
            statusStr = "Nothing recorded";
        }
        else {
            saveMeta(recPath, recFreq, recSampleRate, recSamples);
            statusStr = "Recorded " + durationStr(recSamples, recSampleRate);
            lastFile = std::filesystem::path(recPath).filename().string();
            config.acquire();
            config.conf["lastFile"] = lastFile;
            config.release(true);
        }

        state = STATE_IDLE;
        refreshFiles();
        flog::info("HackRFTx: recording stopped, {0} samples", (uint64_t)recSamples);
    }

    static void recordHandler(dsp::complex_t* data, int count, void* ctx) {
        HackRFTxModule* _this = (HackRFTxModule*)ctx;
        _this->recWriter.write((float*)data, count);
        _this->recSamples += count;
    }

    // ---------------------------------------------------------------- transmit

    void startTransmit() {
        if (state != STATE_IDLE) { return; }
        if (fileList.empty()) {
            showError("No recording selected.");
            return;
        }
        if (txThread.joinable()) { txThread.join(); }

        std::string path = (std::filesystem::path(recDir) / fileList[txFileId]).string();
        double freq = txFreq;
        int vga = txVga;
        bool useAmp = amp;

        txStopReq = false;
        txFinished = false;
        txPos = 0;
        txTotal = 0;

        state = STATE_LOADING;
        statusStr = "Loading";

        txThread = std::thread(&HackRFTxModule::txWorker, this, path, freq, vga, useAmp);
    }

    void stopTransmit(bool wait = false) {
        txStopReq = true;
        if (wait && txThread.joinable()) { txThread.join(); }
    }

    void txWorker(std::string path, double freq, int vga, bool useAmp) {
        WavInfo info;
        std::string err;

        // 1. Parse and preload the recording
        if (!parseWav(path, info, err)) {
            fail("Could not read recording:\n" + err);
            return;
        }
        bool isFloat = (info.format == 3 && info.bitsPerSample == 32);
        bool isInt16 = (info.format == 1 && info.bitsPerSample == 16);
        if (info.channels != 2 || (!isFloat && !isInt16)) {
            fail("Unsupported recording format.\nOnly 2-channel 16-bit PCM or 32-bit float WAV can be transmitted.");
            return;
        }
        if (info.sampleRate < HACKRF_MIN_TX_SR || info.sampleRate > HACKRF_MAX_TX_SR) {
            fail("Recording is " + srStr((double)info.sampleRate) + ".\n"
                 "The HackRF can only transmit between 2 and 20 MS/s.");
            return;
        }
        if (info.dataSize == 0) {
            fail("Recording is empty.");
            return;
        }

        uint64_t frames = info.dataSize / (info.channels * (info.bitsPerSample / 8));
        if (frames * 2 > MAX_TX_BYTES) {
            fail("Recording is too long to transmit in one go (limit 1 GB of IQ data).");
            return;
        }

        if (!loadWavToInt8(path, info, err)) {
            fail("Could not load recording:\n" + err);
            return;
        }

        int sr = (int)info.sampleRate;
        txFileSampleRate = sr;
        txTotal = txData.size();

        if (txStopReq) { finishTx(); return; }

        // 2. HackRF is half duplex: release the receiver before opening the device
        bool wasPlaying = gui::mainWindow.sdrIsRunning();
        if (wasPlaying) { gui::mainWindow.setPlayState(false); }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // 3. Open the device
        std::string serial = firstSerial();
        if (serial.empty()) {
            resumeRx(wasPlaying);
            fail("No HackRF device found.");
            return;
        }

        hackrf_device* dev = NULL;
        hackrf_error hErr = (hackrf_error)hackrf_open_by_serial(serial.c_str(), &dev);
        if (hErr != HACKRF_SUCCESS || !dev) {
            resumeRx(wasPlaying);
            fail(std::string("Could not open HackRF: ") + hackrf_error_name(hErr));
            return;
        }

        // 4. Configure for transmit
        hackrf_set_sample_rate(dev, sr);
        hackrf_set_baseband_filter_bandwidth(dev, hackrf_compute_baseband_filter_bw(sr));
        hackrf_set_freq(dev, (uint64_t)freq);
        hackrf_set_txvga_gain(dev, vga);
        hackrf_set_amp_enable(dev, useAmp ? 1 : 0);

        hErr = (hackrf_error)hackrf_start_tx(dev, txCallback, this);
        if (hErr != HACKRF_SUCCESS) {
            hackrf_close(dev);
            resumeRx(wasPlaying);
            fail(std::string("Could not start transmission: ") + hackrf_error_name(hErr));
            return;
        }

        {
            std::lock_guard<std::mutex> lck(stateMtx);
            state = STATE_TRANSMITTING;
            statusStr = "Transmitting";
        }
        flog::info("HackRFTx: transmitting {0} at {1} Hz, {2} S/s, VGA {3} dB, amp {4}",
                   path, freq, sr, vga, useAmp ? 1 : 0);

        // 5. Wait for the buffer to drain, or for the user to abort
        while (!txFinished && !txStopReq) {
            if (hackrf_is_streaming(dev) != HACKRF_TRUE) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        hackrf_stop_tx(dev);
        hackrf_close(dev);

        resumeRx(wasPlaying);

        {
            std::lock_guard<std::mutex> lck(stateMtx);
            statusStr = txStopReq ? "Transmission aborted" : "Transmission finished";
        }
        finishTx();
        flog::info("HackRFTx: transmission ended");
    }

    static int txCallback(hackrf_transfer* transfer) {
        HackRFTxModule* _this = (HackRFTxModule*)transfer->tx_ctx;
        if (_this->txStopReq) { return -1; }

        size_t want = transfer->valid_length;
        uint64_t remain = _this->txTotal - _this->txPos;

        if (remain == 0) {
            _this->txFinished = true;
            return -1;
        }

        size_t n = (size_t)std::min<uint64_t>(want, remain);
        memcpy(transfer->buffer, _this->txData.data() + _this->txPos, n);
        _this->txPos += n;

        // Pad the tail of the last block with silence
        if (n < want) {
            memset(transfer->buffer + n, 0, want - n);
            _this->txFinished = true;
        }

        return 0;
    }

    void finishTx() {
        std::lock_guard<std::mutex> lck(stateMtx);
        state = STATE_IDLE;
    }

    void resumeRx(bool wasPlaying) {
        if (!wasPlaying) { return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gui::mainWindow.setPlayState(true);
    }

    void fail(std::string msg) {
        {
            std::lock_guard<std::mutex> lck(stateMtx);
            state = STATE_IDLE;
            statusStr = "Error";
        }
        flog::error("HackRFTx: {0}", msg);
        showError(msg);
    }

    // ---------------------------------------------------------------- helpers

    static std::string timestamp() {
        char buf[32];
        time_t t = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
        return std::string(buf);
    }

    static std::string durationStr(uint64_t samples, double sr) {
        char buf[64];
        double secs = (sr > 0.0) ? (samples / sr) : 0.0;
        snprintf(buf, sizeof(buf), "%.2f s", secs);
        return std::string(buf);
    }

    static std::string srStr(double sr) {
        char buf[64];
        if (sr >= 1e6) { snprintf(buf, sizeof(buf), "%.2f MS/s", sr / 1e6); }
        else if (sr >= 1e3) { snprintf(buf, sizeof(buf), "%.1f kS/s", sr / 1e3); }
        else { snprintf(buf, sizeof(buf), "%.0f S/s", sr); }
        return std::string(buf);
    }

    // Keep a recording inside both the source bandwidth and the HackRF TX range.
    // A source narrower than 2 MS/s wins: the file is still usable for analysis,
    // it just cannot be transmitted 1:1.
    static double clampToTxRange(double sr, double srcSr) {
        double maxSr = std::min(srcSr, (double)HACKRF_MAX_TX_SR);
        double minSr = std::min((double)HACKRF_MIN_TX_SR, maxSr);
        return std::clamp(sr, minSr, maxSr);
    }

    // Open a directory in the system file manager.
    //
    // Windows: ShellExecuteW. Going through std::system() spawns a cmd.exe
    // console (visible flash) and blocks the UI thread until it returns, and
    // Explorer misparses paths that mix '/' with the native separator, which is
    // what "opens the wrong folder" looked like.
    // POSIX: a double fork plus exec, so the child is reparented and leaves no
    // zombie behind and the UI never waits for the file manager to exit.
    static void openFolder(const std::string& dir) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::filesystem::path p = std::filesystem::u8path(dir);
        std::filesystem::path abs = std::filesystem::absolute(p, ec);
        if (!ec) { p = abs; }

        flog::info("HackRFTx: opening folder '{0}'", p.string());

#ifdef _WIN32
        std::wstring native = p.make_preferred().wstring();
        ShellExecuteW(NULL, L"open", native.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
        const char* opener = "open";
#else
        const char* opener = "xdg-open";
#endif
        std::string arg = p.string();
        pid_t pid = fork();
        if (pid == 0) {
            if (fork() != 0) { _exit(0); }
            execlp(opener, opener, arg.c_str(), (char*)NULL);
            _exit(127);
        }
        if (pid > 0) { waitpid(pid, NULL, 0); }
#endif
    }

    // Sample rate the next recording would use, for the menu preview
    double previewSampleRate() {
        double srcSr = sigpath::iqFrontEnd.getSampleRate();
        if (srcSr <= 0.0) { return 0.0; }
        if (recSource == REC_SRC_RADIO) {
            std::string n = gui::waterfall.selectedVFO;
            if (n.empty() || !sigpath::vfoManager.vfoExists(n)) { return 0.0; }
            return clampToTxRange(sigpath::vfoManager.getBandwidth(n), srcSr);
        }
        if (recSource == REC_SRC_MANUAL) { return clampToTxRange(sampleRate, srcSr); }
        return clampToTxRange(srcSr, srcSr);
    }

    // Centre frequency the next recording would use
    double previewCenterFreq() {
        double c = gui::waterfall.getCenterFrequency();
        if (recSource != REC_SRC_RADIO) { return c; }
        std::string n = gui::waterfall.selectedVFO;
        if (n.empty() || !sigpath::vfoManager.vfoExists(n)) { return c; }
        return c + sigpath::vfoManager.getOffset(n);
    }

    static std::string freqStr(double freq) {
        char buf[64];
        if (freq >= 1e9) { snprintf(buf, sizeof(buf), "%.6f GHz", freq / 1e9); }
        else if (freq >= 1e6) { snprintf(buf, sizeof(buf), "%.6f MHz", freq / 1e6); }
        else if (freq >= 1e3) { snprintf(buf, sizeof(buf), "%.3f kHz", freq / 1e3); }
        else { snprintf(buf, sizeof(buf), "%.0f Hz", freq); }
        return std::string(buf);
    }

    static std::string firstSerial() {
        std::string out;
        hackrf_device_list_t* list = hackrf_device_list();
        if (!list) { return out; }
        for (int i = 0; i < list->devicecount; i++) {
            if (list->serial_numbers[i]) { out = list->serial_numbers[i]; break; }
        }
        hackrf_device_list_free(list);
        return out;
    }

    static bool parseWav(const std::string& path, WavInfo& info, std::string& err) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { err = "cannot open " + path; return false; }

        char riff[4], wave[4];
        uint32_t riffSize = 0;
        f.read(riff, 4);
        f.read((char*)&riffSize, 4);
        f.read(wave, 4);
        if (f.gcount() != 4 || memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4)) {
            err = "not a RIFF/WAVE file";
            return false;
        }

        while (f) {
            char id[4];
            uint32_t sz = 0;
            f.read(id, 4);
            if (f.gcount() != 4) { break; }
            f.read((char*)&sz, 4);
            if (f.gcount() != 4) { break; }

            std::streampos next = f.tellg();
            next += (std::streamoff)(sz + (sz & 1));

            if (!memcmp(id, "fmt ", 4)) {
                uint16_t audioFormat = 0, ch = 0, blockAlign = 0, bits = 0;
                uint32_t sr = 0, byteRate = 0;
                f.read((char*)&audioFormat, 2);
                f.read((char*)&ch, 2);
                f.read((char*)&sr, 4);
                f.read((char*)&byteRate, 4);
                f.read((char*)&blockAlign, 2);
                f.read((char*)&bits, 2);
                info.format = audioFormat;
                info.channels = ch;
                info.sampleRate = sr;
                info.bitsPerSample = bits;
                if (sz > 16) { f.seekg(next); }
            }
            else if (!memcmp(id, "data", 4)) {
                info.dataOffset = (uint64_t)f.tellg();
                info.dataSize = sz;
                break;
            }
            else {
                f.seekg(next);
            }
        }

        if (!info.channels || !info.sampleRate || !info.bitsPerSample || !info.dataSize) {
            err = "missing fmt or data chunk";
            return false;
        }
        return true;
    }

    bool loadWavToInt8(const std::string& path, const WavInfo& info, std::string& err) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { err = "cannot open " + path; return false; }
        f.seekg((std::streamoff)info.dataOffset);

        bool isFloat = (info.format == 3);
        int bytesPerFrame = (int)info.channels * (int)(info.bitsPerSample / 8);
        uint64_t frames = info.dataSize / bytesPerFrame;
        txData.resize((size_t)(frames * 2));

        const int CHUNK_FRAMES = 65536;
        std::vector<float> fbuf((size_t)CHUNK_FRAMES * 2);
        std::vector<int16_t> ibuf;
        if (!isFloat) { ibuf.resize((size_t)CHUNK_FRAMES * 2); }

        uint64_t done = 0;
        while (done < frames) {
            if (txStopReq) { return true; }
            int n = (int)std::min<uint64_t>(CHUNK_FRAMES, frames - done);
            std::streamsize want = (std::streamsize)n * bytesPerFrame;

            f.read((char*)(isFloat ? (void*)fbuf.data() : (void*)ibuf.data()), want);
            if (f.gcount() != want) {
                err = "unexpected end of file";
                return false;
            }

            if (!isFloat) {
                // int16 -> float -> int8, matching HackRF's native sample format
                volk_16i_s32f_convert_32f(fbuf.data(), ibuf.data(), 32768.0f, n * 2);
            }
            volk_32f_s32f_convert_8i((int8_t*)txData.data() + done * 2, fbuf.data(), 128.0f, n * 2);
            done += n;
        }
        return true;
    }

    void saveMeta(const std::string& wavPath, double freq, int sr, uint64_t samples) {
        std::filesystem::path p(wavPath);
        p.replace_extension(".json");

        std::ofstream f(p);
        if (!f) { return; }
        f << "{\n";
        f << "  \"frequency\": " << (uint64_t)freq << ",\n";
        f << "  \"sampleRate\": " << sr << ",\n";
        f << "  \"samples\": " << samples << ",\n";
        f << "  \"duration\": " << (sr > 0 ? (double)samples / sr : 0.0) << ",\n";
        f << "  \"timestamp\": \"" << timestamp() << "\"\n";
        f << "}\n";
    }

    bool loadMeta(const std::string& wavPath, double& freq) {
        std::filesystem::path p(wavPath);
        p.replace_extension(".json");
        std::ifstream f(p);
        if (!f) { return false; }
        try {
            json j;
            f >> j;
            if (j.contains("frequency")) {
                freq = (double)j["frequency"].get<uint64_t>();
                return true;
            }
        }
        catch (...) {}
        return false;
    }

    void refreshFiles() {
        fileList.clear();
        fileNamesTxt.clear();

        std::error_code ec;
        if (!std::filesystem::is_directory(recDir, ec)) {
            fileNamesTxt = "(no recordings)\0";
            txFileId = 0;
            return;
        }

        std::vector<std::filesystem::path> paths;
        for (const auto& e : std::filesystem::directory_iterator(recDir, ec)) {
            if (!e.is_regular_file()) { continue; }
            if (e.path().extension() != ".wav") { continue; }
            paths.push_back(e.path());
        }
        std::sort(paths.begin(), paths.end());

        int id = 0;
        for (auto& p : paths) {
            std::string fname = p.filename().string();
            double freq = 0.0;
            bool hasFreq = loadMeta(p.string(), freq);

            fileList.push_back(fname);
            fileNamesTxt += fname;
            if (hasFreq) { fileNamesTxt += "  [" + freqStr(freq) + "]"; }
            fileNamesTxt += '\0';

            if (fname == lastFile) { txFileId = id; }
            id++;
        }

        if (fileList.empty()) {
            fileNamesTxt = "(no recordings)\0";
            txFileId = 0;
        }
        else {
            // Sync the transmit frequency with the selected recording
            double f = 0.0;
            if (loadMeta((std::filesystem::path(recDir) / fileList[txFileId]).string(), f)) {
                txFreq = f;
            }
        }
    }

    void selectFile(int id) {
        if (id < 0 || id >= (int)fileList.size()) { return; }
        txFileId = id;
        lastFile = fileList[id];

        double f = 0.0;
        if (loadMeta((std::filesystem::path(recDir) / lastFile).string(), f)) { txFreq = f; }

        config.acquire();
        config.conf["lastFile"] = lastFile;
        config.conf["txFreq"] = txFreq;
        config.release(true);
    }

    void showError(std::string msg) {
        errorStr = msg;
        showErrorDialog = true;
    }

    // -------------------------------------------------------------------- menu

    static void menuHandler(void* ctx) {
        HackRFTxModule* _this = (HackRFTxModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        ImGui::GenericDialog("##hackrf_tx_err_", _this->showErrorDialog, GENERIC_DIALOG_BUTTONS_OK, [=]() {
            ImGui::TextWrapped("%s", _this->errorStr.c_str());
        });

        if (!_this->enabled) { ImGui::BeginDisabled(); }

        TxState st = _this->state.load();
        bool busy = (st != STATE_IDLE);

        // ---- Recording -------------------------------------------------------
        ImGui::TextUnformatted("Record");
        ImGui::Separator();

        if (st == STATE_RECORDING) { ImGui::BeginDisabled(); }

        ImGui::LeftLabel("Source");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##hackrf_tx_recsrc_", _this->name), &_this->recSourceId, REC_SOURCE_TXT)) {
            _this->recSource = (RecSource)_this->recSourceId;
            config.acquire();
            config.conf["recSource"] = _this->recSourceId;
            config.release(true);
        }

        if (_this->recSource == REC_SRC_MANUAL) {
            ImGui::LeftLabel("Sample rate");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##hackrf_tx_sr_", _this->name), &_this->sampleRateId, SAMPLE_RATES_TXT)) {
                _this->sampleRate = SAMPLE_RATES[_this->sampleRateId];
                config.acquire();
                config.conf["sampleRate"] = _this->sampleRate;
                config.release(true);
            }
        }
        else {
            double psr = _this->previewSampleRate();
            ImGui::LeftLabel("Sample rate");
            ImGui::FillWidth();
            if (psr <= 0.0) {
                ImGui::TextUnformatted("no radio selected");
            }
            else if (psr < HACKRF_MIN_TX_SR) {
                // A narrow source limits the recording below the TX range
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f), "%s (below TX min)",
                                   srStr(psr).c_str());
            }
            else {
                ImGui::TextUnformatted(srStr(psr).c_str());
            }
        }

        if (_this->recSource == REC_SRC_RADIO) {
            ImGui::TextWrapped("Clamped to the HackRF minimum of 2 MS/s, so the slice is wider "
                               "than the radio bandwidth but centred on it.");
        }

        ImGui::LeftLabel("Centre");
        ImGui::FillWidth();
        ImGui::TextUnformatted(freqStr(_this->previewCenterFreq()).c_str());

        ImGui::LeftLabel("Depth");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##hackrf_tx_depth_", _this->name), &_this->recDepthId, REC_DEPTH_TXT)) {
            config.acquire();
            config.conf["recDepth"] = _this->recDepthId;
            config.release(true);
        }

        if (st == STATE_RECORDING) { ImGui::EndDisabled(); }

        if (st == STATE_IDLE) {
            if (ImGui::Button(CONCAT("Record##hackrf_tx_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->startRecord();
            }
        }
        else if (st == STATE_RECORDING) {
            if (ImGui::Button(CONCAT("Stop recording##hackrf_tx_recstop_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stopRecord();
            }
        }
        else {
            ImGui::BeginDisabled();
            ImGui::Button(CONCAT("Record##hackrf_tx_rec_", _this->name), ImVec2(menuWidth, 0));
            ImGui::EndDisabled();
        }

        if (st == STATE_RECORDING) {
            ImGui::Text("Recording  %s", durationStr(_this->recSamples, _this->recSampleRate).c_str());
            ImGui::Text("Centre     %s", freqStr(_this->recFreq).c_str());
            ImGui::Text("Rate       %s", srStr(_this->recSampleRate).c_str());
            ImGui::TextWrapped("Keep the recording selector in place; the centre frequency is "
                               "captured when the recording starts.");
        }

        ImGui::Spacing();

        // ---- Recording file --------------------------------------------------
        ImGui::TextUnformatted("Recording file");
        ImGui::Separator();

        bool canPick = (st == STATE_IDLE);
        if (!canPick) { ImGui::BeginDisabled(); }
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##hackrf_tx_file_", _this->name), &_this->txFileId, _this->fileNamesTxt.c_str())) {
            _this->selectFile(_this->txFileId);
        }
        if (!canPick) { ImGui::EndDisabled(); }

        if (ImGui::Button(CONCAT("Refresh##hackrf_tx_refr_", _this->name))) {
            _this->refreshFiles();
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Open folder##hackrf_tx_open_", _this->name))) {
            openFolder(_this->recDir);
        }

        ImGui::Spacing();

        // ---- Transmit --------------------------------------------------------
        ImGui::TextUnformatted("Transmit");
        ImGui::Separator();

        if (busy) { ImGui::BeginDisabled(); }

        ImGui::LeftLabel("Frequency");
        ImGui::FillWidth();
        double freqMHz = _this->txFreq / 1e6;
        if (ImGui::InputDouble(CONCAT("##hackrf_tx_freq_", _this->name), &freqMHz, 0.001, 0.1, "%.6f")) {
            _this->txFreq = freqMHz * 1e6;
            config.acquire();
            config.conf["txFreq"] = _this->txFreq;
            config.release(true);
        }

        ImGui::LeftLabel("TX VGA gain");
        ImGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##hackrf_tx_vga_", _this->name), &_this->txVga, 0, 47, "%d dB")) {
            config.acquire();
            config.conf["txVgaGain"] = _this->txVga;
            config.release(true);
        }

        if (ImGui::Checkbox(CONCAT("Amp enabled##hackrf_tx_amp_", _this->name), &_this->amp)) {
            config.acquire();
            config.conf["amp"] = _this->amp;
            config.release(true);
        }

        if (busy) { ImGui::EndDisabled(); }

        if (st == STATE_IDLE) {
            if (ImGui::Button(CONCAT("Transmit##hackrf_tx_tx_", _this->name), ImVec2(menuWidth, 0))) {
                _this->startTransmit();
            }
        }
        else if (st == STATE_TRANSMITTING || st == STATE_LOADING) {
            if (ImGui::Button(CONCAT("Stop transmission##hackrf_tx_txstop_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stopTransmit();
            }
        }

        // ---- Status ----------------------------------------------------------
        ImGui::Spacing();
        if (st == STATE_TRANSMITTING && _this->txTotal > 0) {
            float progress = (float)((double)_this->txPos / (double)_this->txTotal);
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "%.1f%%  (%s / %s)",
                     progress * 100.0f,
                     durationStr(_this->txPos / 2, _this->txSampleRateOfFile()).c_str(),
                     durationStr(_this->txTotal / 2, _this->txSampleRateOfFile()).c_str());
            ImGui::ProgressBar(progress, ImVec2(menuWidth, 0), overlay);
        }
        else if (st == STATE_LOADING) {
            ImGui::TextUnformatted("Loading recording...");
        }
        else {
            ImGui::Text("Status: %s", _this->statusStr.c_str());
        }

        if (!_this->enabled) { ImGui::EndDisabled(); }
    }

    // Sample rate of the loaded file, for progress display only
    double txSampleRateOfFile() {
        return (double)(int)txFileSampleRate;
    }

    // ------------------------------------------------------------------ members

    std::string name;
    const char* displayName = "HackRF TX";
    bool enabled = true;

    std::string recDir;
    std::string recPath;
    std::string lastFile;

    std::atomic<TxState> state{ STATE_IDLE };
    std::mutex stateMtx;

    // Recording
    int sampleRateId = 0;
    int sampleRate = 2000000;
    int recDepthId = REC_DEPTH_INT16;
    int recSourceId = REC_SRC_WATERFALL;
    RecSource recSource = REC_SRC_WATERFALL;
    double recSourceSr = 0.0;
    VFOManager::VFO* recVfo = NULL;
    wav::Writer recWriter;
    dsp::sink::Handler<dsp::complex_t> recHandler;
    std::atomic<uint64_t> recSamples{ 0 };
    double recFreq = 0.0;
    int recSampleRate = 2000000;

    // Transmit
    std::thread txThread;
    std::atomic<bool> txStopReq{ false };
    std::atomic<bool> txFinished{ false };
    std::atomic<uint64_t> txPos{ 0 };
    std::atomic<uint64_t> txTotal{ 0 };
    std::atomic<int> txFileSampleRate{ 2000000 };
    std::vector<int8_t> txData;
    double txFreq = 0.0;
    int txVga = 20;
    bool amp = false;

    // File selection
    std::vector<std::string> fileList;
    std::string fileNamesTxt = "(no recordings)\0";
    int txFileId = 0;

    // UI
    std::string statusStr = "Idle";
    std::string errorStr;
    bool showErrorDialog = false;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["sampleRate"] = 2000000;
    def["recDepth"] = 0;
    def["recSource"] = 0;
    def["txVgaGain"] = 20;
    def["amp"] = false;
    def["txFreq"] = 0.0;
    def["lastFile"] = "";
    config.setPath(core::args["root"].s() + "/hackrf_tx_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HackRFTxModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HackRFTxModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
