#include "pch.h"
#include "Settings.h"
#include "GameInfo.h"
#include <map>
#include <algorithm>

namespace
{
    enum class Kind { Bool, Float, Choice };
    struct Key
    {
        const char* name;          //!< "section.key", lower case
        Kind kind;
        const char* def;           //!< default, as it would be written in the file
        float lo, hi;              //!< range of a Float
        const char* choices;       //!< "a|b" for a Choice
    };
    // The keys the file may hold. A missing key takes its default; the shipped DishonoredVR.ini lists them all, with
    // the same defaults, except [Advanced] WriteStatusFile, which defaults to 1 here (a development setup has no ini)
    // and is 0 in the file the players get.
    const Key kKeys[] = {
        { "controls.turnmode",          Kind::Choice, "smooth", 0, 0, "smooth|snap" },
        { "controls.smoothturnspeed",   Kind::Float,  "120", 30, 360, nullptr },
        { "controls.snapturnangle",     Kind::Float,  "45", 10, 90, nullptr },
        { "controls.quickslotholdtime", Kind::Float,  "0.5", 0.15f, 2, nullptr },
        { "controls.layout",            Kind::Choice, "default", 0, 0, "default|classic" },
        { "controls.haptics",           Kind::Bool,   "1", 0, 0, nullptr },
        { "controls.controllerpitchoffset", Kind::Float, "0", -60, 60, nullptr },
        { "controls.focusgamewindow",   Kind::Bool,   "1", 0, 0, nullptr },
        { "gameplay.swordgestures",     Kind::Bool,   "1", 0, 0, nullptr },
        { "gameplay.throwgestures",     Kind::Bool,   "1", 0, 0, nullptr },
        { "gameplay.grabobjects",       Kind::Bool,   "1", 0, 0, nullptr },
        { "display.reticle",            Kind::Bool,   "1", 0, 0, nullptr },
        { "display.aimdot",             Kind::Bool,   "1", 0, 0, nullptr },
        { "display.wristhud",           Kind::Bool,   "1", 0, 0, nullptr },
        { "display.wristhudsize",       Kind::Float,  "8.5", 4, 15, nullptr },
        { "display.stereohud",          Kind::Bool,   "1", 0, 0, nullptr },
        { "display.stereohudwidth",     Kind::Float,  "60", 35, 90, nullptr },
        // 0 mono, 1 synchronised stereo, 2 the alternating stereo of 1.0 (whose ini had 1 / true for that one: it gets
        // the synchronised one now, which is what 1 meant to the player)
        { "display.stereo3d",           Kind::Choice, "1", 0, 0, "0|1|2|true|false|on|off|yes|no" },
        { "display.renderscale",        Kind::Float,  "1", 0, 2, nullptr },
        { "display.highresolution",     Kind::Bool,   "0", 0, 0, nullptr },   // 0.1.0's ini; replaced by RenderScale
        { "display.disablemotionblur",  Kind::Bool,   "1", 0, 0, nullptr },
        { "display.disablemetaasw",     Kind::Bool,   "1", 0, 0, nullptr },   // Meta's ASW off for this game (MetaAsw)
        { "display.menuscreen",         Kind::Bool,   "1", 0, 0, nullptr },
        { "display.menuscreensize",     Kind::Float,  "62", 30, 110, nullptr },
        { "display.menuscreendistance", Kind::Float,  "2.4", 0.8f, 10, nullptr },
        { "display.menubackground",     Kind::Float,  "0.22", 0, 1, nullptr },
        { "display.cutsceneletterbox",  Kind::Bool,   "0", 0, 0, nullptr },
        { "display.cutscenearms",       Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.unlockeverything",    Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.infinitemana",        Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.infiniteammo",        Kind::Bool,   "0", 0, 0, nullptr },
        { "cheats.infinitehealth",      Kind::Bool,   "0", 0, 0, nullptr },
        { "advanced.writestatusfile",   Kind::Bool,   "1", 0, 0, nullptr },
        { "advanced.logcontrollerinput", Kind::Bool,  "1", 0, 0, nullptr },
        { "advanced.keepaliveframes",   Kind::Bool,   "1", 0, 0, nullptr },
        { "advanced.newsessionafter",   Kind::Float,  "5", 0, 120, nullptr },
    };

    std::map<std::string, std::string> g_Val;     //!< validated values, every key present
    bool g_FileFound = false;
    int g_Read = 0, g_Bad = 0;

    std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; }
    std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    bool ParseBool(const std::string& v, bool& out)
    {
        const std::string l = Lower(v);
        if (l == "1" || l == "true" || l == "on" || l == "yes") { out = true; return true; }
        if (l == "0" || l == "false" || l == "off" || l == "no") { out = false; return true; }
        return false;
    }
    //! The value for a key as the file has it, checked against the key's type; false = not usable.
    bool Validate(const Key& k, const std::string& raw, std::string& out)
    {
        if (k.kind == Kind::Bool) { bool b; if (!ParseBool(raw, b)) return false; out = b ? "1" : "0"; return true; }
        if (k.kind == Kind::Float)
        {
            // Decimals with a point; a comma (how a Spanish or German Windows writes them) is taken as a point too.
            std::string r = raw; std::replace(r.begin(), r.end(), ',', '.');
            char* end = nullptr; const double d = strtod(r.c_str(), &end);
            if (end == r.c_str() || Trim(end).size()) return false;
            const float f = (float)(d < k.lo ? k.lo : d > k.hi ? k.hi : d);
            if (f != (float)d) LOGI("Settings: {} = {} is outside {}..{}: using {}", k.name, raw, k.lo, k.hi, f);
            out = fmt::format("{}", f); return true;
        }
        const std::string l = Lower(raw), all = std::string("|") + k.choices + "|";
        if (l.empty() || l.find('|') != std::string::npos || all.find("|" + l + "|") == std::string::npos) return false;
        out = l; return true;
    }
    //! Read-only after Load (several threads ask): never insert here.
    const std::string& V(const char* key)
    {
        static const std::string none;
        auto it = g_Val.find(key);
        if (it != g_Val.end()) return it->second;
        for (const Key& k : kKeys) if (!strcmp(k.name, key)) { static thread_local std::string d; d = k.def; return d; }
        return none;
    }
    bool B(const char* key) { return V(key) == "1"; }
    float F(const char* key) { return (float)atof(V(key).c_str()); }
}

void Settings::Load()
{
    for (const Key& k : kKeys) g_Val[k.name] = k.def;
    const std::wstring path = GameInfo::GameDir() + L"\\DishonoredVR.ini";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { LOGI("Settings: no DishonoredVR.ini in the game folder: every setting at its default"); return; }
    g_FileFound = true;
    std::string text; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    if (text.size() >= 2 && (unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE)
        LOGI("Settings: DishonoredVR.ini is saved as UTF-16 and cannot be read: save it as UTF-8 or ANSI (Notepad: Save As > Encoding)");
    std::string section; int lineNo = 0; size_t pos = 0;
    while (pos <= text.size())
    {
        size_t eol = text.find('\n', pos); if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos); pos = eol + 1; lineNo++;
        // A comment starts with ; or #, at the start of a line or after the value.
        const size_t c = line.find_first_of(";#"); if (c != std::string::npos) line.erase(c);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[')
        {
            const size_t close = line.find(']');
            section = Lower(Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { LOGI("Settings: line {} ignored (no '='): {}", lineNo, line); g_Bad++; continue; }
        const std::string key = section + "." + Lower(Trim(line.substr(0, eq))), raw = Trim(line.substr(eq + 1));
        const Key* k = nullptr;
        for (const Key& kk : kKeys) if (key == kk.name) { k = &kk; break; }
        if (!k) { LOGI("Settings: line {}: unknown setting '{}' ignored", lineNo, key); g_Bad++; continue; }
        std::string v;
        if (!Validate(*k, raw, v)) { LOGI("Settings: line {}: {} = '{}' is not a valid value: keeping the default '{}'", lineNo, k->name, raw, k->def); g_Bad++; continue; }
        g_Val[k->name] = v; g_Read++;
    }
    LOGI("Settings: DishonoredVR.ini read: {} settings, {} lines ignored", g_Read, g_Bad);
}

std::vector<std::string> Settings::StartupCommands()
{
    std::vector<std::string> c;
    c.push_back(V("controls.turnmode") == "snap" ? fmt::format("vr snap {}", F("controls.snapturnangle")) : "vr snap 0");
    c.push_back(fmt::format("vr turnspeed {}", F("controls.smoothturnspeed")));
    c.push_back(fmt::format("vr quickhold {}", F("controls.quickslotholdtime")));
    c.push_back(V("controls.layout") == "classic" ? "vr quickslots 0" : "vr quickslots 1");
    // the game brought to the front when the controllers are used while it is behind another window (KbmInject)
    c.push_back(B("controls.focusgamewindow") ? "vr autofocus 1" : "vr autofocus 0");
    // the menu screen is there from the title screen on, before any game is loaded (ScreenMode)
    c.push_back(B("display.menuscreen") ? "vr screen 1" : "vr screen 0");
    c.push_back(fmt::format("vr screencfg size {}", F("display.menuscreensize")));
    c.push_back(fmt::format("vr screencfg dist {}", F("display.menuscreendistance")));
    c.push_back(fmt::format("vr screencfg backdrop {}", F("display.menubackground")));
    // cutscenes in the engine: the game's letterbox, and whose arms
    c.push_back(B("display.cutsceneletterbox") ? "vr cinebars 0" : "vr cinebars 1");
    c.push_back(B("display.cutscenearms") ? "vr cinearms 1" : "vr cinearms 0");
    // the headset's frames when the game or the runtime stalls
    c.push_back(B("advanced.keepaliveframes") ? "vr keepalive 1" : "vr keepalive 0");
    c.push_back(fmt::format("vr xrkick {}", F("advanced.newsessionafter")));
    c.push_back(B("display.disablemetaasw") ? "vr metaasw 1" : "vr metaasw 0");
    return c;
}

std::vector<std::string> Settings::PackageCommands()
{
    std::vector<std::string> c;
    c.push_back(B("gameplay.swordgestures") ? "vr swing 7" : "vr swing 0");
    c.push_back(B("gameplay.throwgestures") ? "vr throw 1" : "vr throw 0");
    c.push_back(B("gameplay.grabobjects") ? "vr grab 1" : "vr grab 0");
    c.push_back(B("display.reticle") ? "vr reticle 1" : "vr reticle 0");
    c.push_back(B("display.aimdot") ? "vr aimdot 1" : "vr aimdot 0");
    c.push_back(B("display.wristhud") ? "vr wristhud 1" : "vr wristhud 0");
    c.push_back(fmt::format("vr wristcfg size {}", F("display.wristhudsize") / 100.f));   // cm in the file, m in the command
    {
        const std::string& st = V("display.stereo3d");
        if (st == "1" || st == "true" || st == "on" || st == "yes") c.push_back("vr stereo 2");   // both eyes of one instant
        else if (st == "2") c.push_back("vr stereo 1");                                              // alternating eyes
    }
    // Camera motion blur smears the whole view each time the head-driven camera turns: off in VR (the game keeps it
    // in its own config afterwards, as if it had been unticked in the options).
    if (B("display.disablemotionblur")) c.push_back("exec r_skipCameraMotionBlur 1");
    // `vr autoall 1` (the development command) also switches infinite mana, ammo and items on; the ini's own switches
    // decide each one after it, so "everything unlocked" does not have to mean "nothing runs out" (release 1.0: unlocked
    // + infinite mana, ammo and health normal). Same frame, before any tick: an infinite switched off here never acts.
    if (B("cheats.unlockeverything")) c.push_back("vr autoall 1");
    c.push_back(B("cheats.infinitemana") ? "vr mana inf" : "vr mana 0");
    c.push_back(B("cheats.infiniteammo") ? "vr ammo inf" : "vr ammo 0");   // the game's own counter: ammo, gadgets and elixirs
    c.push_back(B("cheats.infiniteammo") ? "vr infitems 1" : "vr infitems 0");   // + the top-up of what was held
    if (B("cheats.infinitehealth")) c.push_back("vr hp inf");
    return c;
}

bool Settings::StatusFile() { return B("advanced.writestatusfile"); }
bool Settings::LogInput() { return B("advanced.logcontrollerinput"); }
bool Settings::Haptics() { return B("controls.haptics"); }
bool Settings::StereoHud() { return B("display.stereohud"); }
float Settings::StereoHudWidth() { return F("display.stereohudwidth"); }
float Settings::RenderScale() { return F("display.renderscale"); }
float Settings::ControllerPitchOffset() { return F("controls.controllerpitchoffset"); }

std::string Settings::Summary()
{
    std::string s = g_FileFound ? "DishonoredVR.ini:" : "defaults (no DishonoredVR.ini):";
    for (const Key& k : kKeys) s += fmt::format(" {}={}", strchr(k.name, '.') + 1, g_Val[k.name]);
    return s;
}

