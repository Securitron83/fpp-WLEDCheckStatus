/*
 * fpp-WLEDCheckStatus — check WLED device power state from FPP command presets.
 *
 * Commands registered:
 *   "WLED - Ensure Power On"   args: IP Address (string), Brightness (-1–255)
 *   "WLED - Turn Off"          args: IP Address (string)
 *
 * "Ensure Power On" replicates the logic in check_wled_status.sh:
 *   Scenario A — Live=true, On=false:
 *       disable live → turn on (+ optional brightness) → re-enable live
 *   Scenario B — Live=false, On=false:
 *       send {"on":true} (+ optional brightness)
 *   Scenario C — On=true, bri=0:
 *       set brightness to target (or 128 if no target given)
 *
 * All curl calls use a 2-second timeout.  Scenario A makes three sequential
 * POSTs with 500 ms pauses between them; worst-case blocking time is ~7 s.
 * This is intentional — the command is designed as a pre-show playlist step
 * that must complete before the playlist advances.
 *
 * Brightness argument: 0–255 to set a specific level, -1 to leave as-is.
 */

// jsoncpp must come before FPP headers
#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#endif

// FPP plugin API
#include <Plugin.h>
#include <commands/Commands.h>
#include <log.h>

// HTTP
#include <curl/curl.h>

// Standard library
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// HTTP helpers (no global curl state — each call owns its handle)
// ---------------------------------------------------------------------------
static size_t writeCallback(char* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t discardCallback(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

static std::string httpGet(const std::string& url) {
    std::string body;
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &body);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? body : std::string{};
}

static bool httpPost(const std::string& url, const std::string& jsonBody) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class WLEDCheckStatusPlugin;

class WLEDEnsurePowerOnCommand : public Command {
public:
    explicit WLEDEnsurePowerOnCommand(WLEDCheckStatusPlugin* plugin);
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override;
private:
    WLEDCheckStatusPlugin* m_plugin;
};

class WLEDTurnOffCommand : public Command {
public:
    explicit WLEDTurnOffCommand(WLEDCheckStatusPlugin* plugin);
    std::unique_ptr<Result> run(const std::vector<std::string>& a) override;
private:
    WLEDCheckStatusPlugin* m_plugin;
};

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------
class WLEDCheckStatusPlugin : public FPPPlugin {
public:
    WLEDCheckStatusPlugin() : FPPPlugin("WLEDCheckStatus") {
        LogInfo(VB_PLUGIN, "WLEDCheckStatus: initialising\n");
        registerCommands();
    }

    ~WLEDCheckStatusPlugin() {
        unregisterCommands();
        LogInfo(VB_PLUGIN, "WLEDCheckStatus: shutdown\n");
    }

    // -----------------------------------------------------------------------
    // Ensure the device at `ip` is powered on and visible.
    // Returns a human-readable status string passed back as the command result.
    // -----------------------------------------------------------------------
    std::string ensurePowerOn(const std::string& ip, int targetBri) {
        const std::string url = "http://" + ip + "/json/state";

        std::string resp = httpGet(url);
        if (resp.empty()) {
            LogWarn(VB_PLUGIN, "WLEDCheckStatus: no response from %s\n", ip.c_str());
            return "No response from " + ip;
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream ss(resp);
        if (!Json::parseFromStream(builder, ss, &root, &errs)) {
            LogWarn(VB_PLUGIN, "WLEDCheckStatus: invalid JSON from %s\n", ip.c_str());
            return "Invalid JSON from " + ip;
        }

        bool isOn   = root["on"].asBool();
        bool isLive = root["live"].asBool();
        int  curBri = root["bri"].asInt();

        // Scenario A: receiving live data but power is off — recovery sequence
        if (isLive && !isOn) {
            LogInfo(VB_PLUGIN, "WLEDCheckStatus: %s LIVE+OFF — recovery sequence\n", ip.c_str());
            httpPost(url, "{\"live\":false}");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            httpPost(url, onPayload(targetBri));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            httpPost(url, "{\"live\":true}");
            LogInfo(VB_PLUGIN, "WLEDCheckStatus: recovery sent to %s\n", ip.c_str());
            return "Recovery sent to " + ip;
        }

        // Scenario B: idle and off — just turn on
        if (!isOn) {
            LogInfo(VB_PLUGIN, "WLEDCheckStatus: %s OFF — sending ON\n", ip.c_str());
            httpPost(url, onPayload(targetBri));
            return "Turned on " + ip;
        }

        // Scenario C: on but brightness is 0 (effectively dark)
        if (curBri == 0) {
            int newBri = targetBri >= 0 ? targetBri : 128;
            LogInfo(VB_PLUGIN, "WLEDCheckStatus: %s bri=0 — setting to %d\n", ip.c_str(), newBri);
            httpPost(url, "{\"bri\":" + std::to_string(newBri) + "}");
            return "Fixed brightness on " + ip;
        }

        LogInfo(VB_PLUGIN, "WLEDCheckStatus: %s OK (on=t live=%s bri=%d)\n",
                ip.c_str(), isLive ? "t" : "f", curBri);
        return "OK — " + ip + " already on";
    }

    bool turnOff(const std::string& ip) {
        LogInfo(VB_PLUGIN, "WLEDCheckStatus: turning off %s\n", ip.c_str());
        return httpPost("http://" + ip + "/json/state", "{\"on\":false}");
    }

private:
    std::vector<std::string> m_registeredCommands;

    static std::string onPayload(int targetBri) {
        if (targetBri >= 0)
            return "{\"on\":true,\"bri\":" + std::to_string(targetBri) + "}";
        return "{\"on\":true}";
    }

    void registerCommands() {
        auto* onCmd  = new WLEDEnsurePowerOnCommand(this);
        auto* offCmd = new WLEDTurnOffCommand(this);
        CommandManager::INSTANCE.addCommand(onCmd);
        CommandManager::INSTANCE.addCommand(offCmd);
        m_registeredCommands = {onCmd->name, offCmd->name};
        LogInfo(VB_PLUGIN, "WLEDCheckStatus: registered %zu commands\n",
                m_registeredCommands.size());
    }

    void unregisterCommands() {
        for (const auto& n : m_registeredCommands)
            CommandManager::INSTANCE.removeCommand(n);
        m_registeredCommands.clear();
    }
};

// ---------------------------------------------------------------------------
// Command constructors
// ---------------------------------------------------------------------------
WLEDEnsurePowerOnCommand::WLEDEnsurePowerOnCommand(WLEDCheckStatusPlugin* plugin)
    : Command("WLED - Ensure Power On",
              "Check a WLED device and turn it on if needed. "
              "Handles live-mode recovery and zero-brightness fix. "
              "Blocks until complete (up to ~7 s in the recovery scenario)."),
      m_plugin(plugin) {
    args.emplace_back("IP Address", "string",
                      "WLED device IP address (e.g. 192.168.1.50)");
    args.emplace_back("Brightness", "int",
                      "Target brightness 0-255. Use -1 to leave brightness unchanged.",
                      true)   // optional
        .setRange(-1, 255)
        .setDefaultValue("-1");
}

WLEDTurnOffCommand::WLEDTurnOffCommand(WLEDCheckStatusPlugin* plugin)
    : Command("WLED - Turn Off",
              "Send a power-off command to a WLED device."),
      m_plugin(plugin) {
    args.emplace_back("IP Address", "string",
                      "WLED device IP address (e.g. 192.168.1.50)");
}

// ---------------------------------------------------------------------------
// Command run() implementations
// ---------------------------------------------------------------------------
std::unique_ptr<Command::Result>
WLEDEnsurePowerOnCommand::run(const std::vector<std::string>& a) {
    if (a.empty() || a[0].empty())
        return std::make_unique<ErrorResult>("IP Address is required");
    int bri = -1;
    if (a.size() >= 2 && !a[1].empty()) {
        try { bri = std::stoi(a[1]); } catch (...) {}
    }
    if (bri < -1 || bri > 255)
        return std::make_unique<ErrorResult>("Brightness out of range (-1 to 255)");
    return std::make_unique<Result>(m_plugin->ensurePowerOn(a[0], bri));
}

std::unique_ptr<Command::Result>
WLEDTurnOffCommand::run(const std::vector<std::string>& a) {
    if (a.empty() || a[0].empty())
        return std::make_unique<ErrorResult>("IP Address is required");
    bool ok = m_plugin->turnOff(a[0]);
    return ok ? std::make_unique<Result>("OK")
              : std::make_unique<ErrorResult>("Failed to reach " + a[0]);
}

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------
extern "C" {

FPPPlugins::Plugin* createPlugin() {
    return new WLEDCheckStatusPlugin();
}

} // extern "C"
