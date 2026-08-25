#include <windows.h>
#pragma comment(lib, "user32.lib")   // GetAsyncKeyState (hotkeys)
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>      // atexit
#include <string>
#include <algorithm>
#include "../include/bbr2_sdk.h"
#include "../src/bbr2_net.h"
#include "../src/bbr2_gui.h"
#include "../src/bbr2_catalog.h"
#include "../thirdparty/imgui/imgui.h"

using namespace bbr2;

// ---- config ---------------------------------------------------------------
struct Cfg {
    bool     host       = false;
    int      port       = 27015;
    char     hostAddr[64] = "";
    int      hostPort   = 27015;
    int      sendHz     = 30;
    uint64_t seed       = 12345;
    int      maxPlayers = net::kMaxPeers;
    // identity
    char     name[24]        = "Player";
    char     vehicle[32]     = "";
    char     driver[32]      = "";
    char     paint[32]       = "";
    char     paintDetail[32] = "";
    char     decal[32]       = "";
    // race (host only)
    char     track[64]   = "";
    char     setting[64] = "";
    char     gameType[24]= "Race";
    int      laps        = 3;
    int      players     = 0;       // grid size; raised to fit the lobby
    bool     mirror      = false;
    bool     autostart   = false;
    int      holdMode    = 4;       // 1 load, 2 grid, 4 pregame (bitmask)
    bool     simpleHud   = false;
    bool     popups      = true;
    char     toastStyle[24] = "Achievement";
    char     toastTag[24]   = "[Multiplayer]";
    bool     skipIntro   = true;
    float    interpDelay = 1.0f;
    // Broadcast which power-up effects are on your car, and mirror everyone
    // else's onto their car here. Off = the old behaviour, where an effect only
    // ever existed on the machine that applied it.
    bool     syncEffects = true;
    // Broadcast which power-ups your car is HOLDING, and force everyone else's
    // copy of your car to hold the same. Separate from syncEffects because it
    // fixes a different symptom: not "the freeze did not land" but "you threw a
    // rocket and I saw a freeze".
    bool     syncPowerUps = true;
    // How long an effect must go unreported before we take it off a remote car.
    // Must comfortably exceed one round trip, or an ordinary hit flickers.
    float    effectGrace = 0.6f;
    // SHOOTER AUTHORITY. When our own simulation lands an effect on our copy of
    // somebody's car, tell them, and let them put it on the car that counts.
    // Off = victim authority: a hit the victim's machine did not also simulate
    // quietly disappears after effectGrace, which is consistent but is exactly
    // the "I clearly hit him and nothing happened" complaint.
    bool     hitClaims   = true;
    // Backstop only. How far back the shooter's picture of us may have been, in
    // seconds, before the claim is refused as a backlog rather than a shot.
    //
    // It is NOT the fairness test - hitMaxDrift is. Deliberately generous, because
    // a high ping alone is not a reason to refuse a hit: a player at 500 ms whose
    // target was barely moving really did hit them. What this catches is the peer
    // whose link hangs for seconds and then delivers the whole burst at once.
    float    hitMaxAge   = 1.5f;
    // How long we hold an unconfirmed effect on a remote car while waiting for
    // the victim to answer our claim. Longer than effectGrace because it covers
    // the retransmit window, their apply, and their next state packet. If it
    // expires, they refused (or every copy was lost) and the effect comes off.
    float    hitTimeout  = 1.5f;
    // Never claim the same effect against the same player twice inside this many
    // seconds. A rate limit on claims, and one half of the pair of backstops that
    // makes any runaway loop terminate rather than merely slow down. The worst it
    // allows is one missed follow-up hit; the worst it prevents is a car that can
    // never be un-frozen.
    float    hitCooldown = 2.0f;
    // THE PLAUSIBILITY GATE, and the reason this is not simply "the shooter is
    // always right". How far the victim may have travelled since the instant the
    // shot claims to have connected, in metres.
    //
    // Aiming is always at a stale ghost - remote cars are drawn a playout buffer
    // behind real time - so some drift is normal and has to be allowed, or the
    // grazing hit at speed (the exact shot this feature exists for) never lands.
    // But a player at 500 ms with heavy loss is aiming at a car that has since
    // left the corner entirely, and that shot should miss. Distance, not time, is
    // the right test: half a second of lag is harmless against a stopped car and
    // fatal against one at 40 m/s, and this measures precisely that.
    //
    // Every accepted claim logs its measured drift, so this can be tuned from
    // real numbers rather than guessed at.
    float    hitMaxDrift = 15.0f;
    // Write picks from the F6 panel straight back to bbr2_mp.ini, so the car you
    // chose is still your car next time you launch.
    bool     autoSave    = true;
    // overlay (see bbr2_gui.h)
    bool     guiEnabled  = true;
    int      guiKey      = VK_F6;
    float    guiScale    = 1.0f;
    bool     guiEatInput = true;
    // diagnostics
    bool     doCountdown = true;
    bool     doClearAlt  = true;
    bool     heartbeat   = false;
    // The grid-slot swap is the fix for players spawning on top of each other,
    // but it moves the human out of roster slot 0, which the engine otherwise
    // always does itself. Switchable in case something downstream cares.
    bool     gridSlots   = true;
    bool     pauseFreeze = false;   // does the pause menu stop the world during a
                                    // networked race? false = keep racing
    bool     randomSeed  = true;    // roll a fresh seed for every race, and send
                                    // it, instead of using the fixed one below
    bool     toastDrain  = true;    // a new toast closes the ones we already put
                                    // up, instead of queueing behind them
    bool     gridFix     = true;    // re-place cars on a spawn order both
                                    // machines agree on, see PlaceGrid()
} g_cfg;

static void Trim(char* s) {
    char* p = s; while (*p==' '||*p=='\t') ++p;
    if (p != s) memmove(s, p, strlen(p)+1);
    size_t n = strlen(s);
    while (n && (s[n-1]=='\r'||s[n-1]=='\n'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
}
static bool Truthy(const char* v) {
    return !_stricmp(v,"1") || !_stricmp(v,"true") || !_stricmp(v,"yes") || !_stricmp(v,"host");
}

// "F6", "f6", "0x75", "117" or "off". Anything unrecognised keeps the default
// and says so, because a silently-ignored hotkey is a bad half hour.
static int ParseVk(const char* v, int fallback) {
    if (!v || !v[0]) return fallback;
    if (!_stricmp(v,"off") || !_stricmp(v,"none") || !_stricmp(v,"0")) return 0;
    if ((v[0]=='f'||v[0]=='F') && v[1] >= '1' && v[1] <= '9') {
        int n = atoi(v+1);
        if (n >= 1 && n <= 12) return VK_F1 + (n-1);
    }
    int n = (int)strtol(v, nullptr, 0);
    if (n > 0 && n < 256) return n;
    Log("[gui] could not read key \"%s\" - keeping the default", v);
    return fallback;
}

// F7..F11 are already read on the game thread by PollHotkeys, and those reads go
// through GetAsyncKeyState, so one press would do both things.
static int RejectHotkeyClash(int vk, int fallback) {
    if (vk >= VK_F7 && vk <= VK_F11) {
        Log("[gui] overlaykey F%d is already a mod hotkey - using the default instead",
            vk - VK_F1 + 1);
        return fallback;
    }
    return vk;
}

static void LoadConfig() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* sl = strrchr(path,'\\')) { sl[1]=0; strcat_s(path, "bbr2_mp.ini"); }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) { Log("[mp] no bbr2_mp.ini - using defaults"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]==';'||line[0]=='#'||line[0]=='[') continue;
        char* eq = strchr(line,'='); if (!eq) continue;
        *eq = 0;
        char k[64]{}, v[160]{};
        strncpy_s(k,line,63); strncpy_s(v,eq+1,159); Trim(k); Trim(v);
        if      (!_stricmp(k,"role"))        g_cfg.host = Truthy(v);
        else if (!_stricmp(k,"port"))        g_cfg.port = atoi(v);
        // "remote" is the old name for the same thing; both accepted.
        else if (!_stricmp(k,"host") ||
                 !_stricmp(k,"remote"))      strncpy_s(g_cfg.hostAddr,v,63);
        else if (!_stricmp(k,"hostport") ||
                 !_stricmp(k,"remoteport"))  g_cfg.hostPort = atoi(v);
        else if (!_stricmp(k,"id"))          { /* ignored - the host assigns it */ }
        else if (!_stricmp(k,"maxplayers"))  g_cfg.maxPlayers = atoi(v);
        else if (!_stricmp(k,"sendhz"))      g_cfg.sendHz = atoi(v);
        else if (!_stricmp(k,"seed")) {
            // "random" / "auto" / 0 all mean "roll one per race". A number pins
            // it, which is what you want when chasing a divergence and need two
            // machines to produce the same race twice.
            if (!_stricmp(v,"random") || !_stricmp(v,"auto") || !_stricmp(v,"rand"))
                g_cfg.randomSeed = true;
            else {
                g_cfg.seed = _strtoui64(v,nullptr,0);
                if (g_cfg.seed == 0) g_cfg.randomSeed = true;
            }
        }
        else if (!_stricmp(k,"randomseed")) g_cfg.randomSeed = Truthy(v);
        else if (!_stricmp(k,"pausefreeze")) g_cfg.pauseFreeze = Truthy(v);
        else if (!_stricmp(k,"name"))        strncpy_s(g_cfg.name,v,23);
        else if (!_stricmp(k,"vehicle"))     strncpy_s(g_cfg.vehicle,v,31);
        else if (!_stricmp(k,"driver"))      strncpy_s(g_cfg.driver,v,31);
        else if (!_stricmp(k,"paint"))       strncpy_s(g_cfg.paint,v,31);
        else if (!_stricmp(k,"paintdetail")) strncpy_s(g_cfg.paintDetail,v,31);
        else if (!_stricmp(k,"decal"))       strncpy_s(g_cfg.decal,v,31);
        else if (!_stricmp(k,"track"))       strncpy_s(g_cfg.track,v,63);
        else if (!_stricmp(k,"setting"))     strncpy_s(g_cfg.setting,v,63);
        else if (!_stricmp(k,"gametype"))    strncpy_s(g_cfg.gameType,v,23);
        else if (!_stricmp(k,"laps"))        g_cfg.laps = atoi(v);
        else if (!_stricmp(k,"players") ||
                 !_stricmp(k,"vehiclecount"))g_cfg.players = atoi(v);
        else if (!_stricmp(k,"mirror"))      g_cfg.mirror = Truthy(v);
        else if (!_stricmp(k,"autostart"))   g_cfg.autostart = Truthy(v);
        else if (!_stricmp(k,"holdstart")) {
            // The words cover five of the eight masks. The overlay can produce
            // any of the eight, so a bare number is accepted too - and an
            // unrecognised word keeps the default rather than quietly meaning 1.
            if      (!_stricmp(v,"off")  || !_stricmp(v,"false"))      g_cfg.holdMode = 0;
            else if (!_stricmp(v,"load") || !_stricmp(v,"loading"))    g_cfg.holdMode = 1;
            else if (!_stricmp(v,"grid"))                              g_cfg.holdMode = 2;
            else if (!_stricmp(v,"both"))                              g_cfg.holdMode = 3;
            else if (!_stricmp(v,"pregame") || !_stricmp(v,"freeze"))  g_cfg.holdMode = 4;
            else if (v[0] >= '0' && v[0] <= '9')                       g_cfg.holdMode = atoi(v) & 7;
            else Log("[mp] holdstart = \"%s\" not understood - keeping %d", v, g_cfg.holdMode);
        }
        else if (!_stricmp(k,"simplehud"))   g_cfg.simpleHud = Truthy(v);
        else if (!_stricmp(k,"popups"))      g_cfg.popups = Truthy(v);
        else if (!_stricmp(k,"toast"))       strncpy_s(g_cfg.toastStyle,v,23);
        else if (!_stricmp(k,"toasttag"))    strncpy_s(g_cfg.toastTag,v,23);
        else if (!_stricmp(k,"skipintro"))   g_cfg.skipIntro = Truthy(v);
        else if (!_stricmp(k,"interpdelay")) g_cfg.interpDelay = (float)atof(v);
        else if (!_stricmp(k,"countdown"))   g_cfg.doCountdown = Truthy(v);
        else if (!_stricmp(k,"clearalternate")) g_cfg.doClearAlt = Truthy(v);
        else if (!_stricmp(k,"heartbeat"))   g_cfg.heartbeat = Truthy(v);
        else if (!_stricmp(k,"gridslots"))   g_cfg.gridSlots = Truthy(v);
        else if (!_stricmp(k,"gridfix"))     g_cfg.gridFix   = Truthy(v);
        else if (!_stricmp(k,"toastdrain"))  g_cfg.toastDrain = Truthy(v);
        else if (!_stricmp(k,"synceffects"))  g_cfg.syncEffects = Truthy(v);
        else if (!_stricmp(k,"syncpowerups")) g_cfg.syncPowerUps = Truthy(v);
        else if (!_stricmp(k,"effectgrace"))  g_cfg.effectGrace = (float)atof(v);
        else if (!_stricmp(k,"hitclaims"))    g_cfg.hitClaims = Truthy(v);
        else if (!_stricmp(k,"hitmaxage"))    g_cfg.hitMaxAge = (float)atof(v);
        else if (!_stricmp(k,"hittimeout"))   g_cfg.hitTimeout = (float)atof(v);
        else if (!_stricmp(k,"hitcooldown"))  g_cfg.hitCooldown = (float)atof(v);
        else if (!_stricmp(k,"hitmaxdrift"))  g_cfg.hitMaxDrift = (float)atof(v);
        else if (!_stricmp(k,"autosave"))    g_cfg.autoSave = Truthy(v);
        else if (!_stricmp(k,"overlay") ||
                 !_stricmp(k,"gui"))         g_cfg.guiEnabled = Truthy(v);
        else if (!_stricmp(k,"overlaykey") ||
                 !_stricmp(k,"guikey"))      g_cfg.guiKey = RejectHotkeyClash(ParseVk(v, VK_F6), VK_F6);
        else if (!_stricmp(k,"overlayscale")||
                 !_stricmp(k,"guiscale"))    g_cfg.guiScale = (float)atof(v);
        else if (!_stricmp(k,"overlayinput"))g_cfg.guiEatInput = Truthy(v);
    }
    fclose(f);
    if (g_cfg.maxPlayers < 2) g_cfg.maxPlayers = 2;
    if (g_cfg.maxPlayers > net::kMaxPeers) g_cfg.maxPlayers = net::kMaxPeers;
    Log("[mp] %s | port=%d host=%s:%d name=%s seed=%llu maxPlayers=%d",
        g_cfg.host ? "HOST" : "client", g_cfg.port,
        g_cfg.hostAddr[0]?g_cfg.hostAddr:"(learn)", g_cfg.hostPort,
        g_cfg.name, (unsigned long long)g_cfg.seed, g_cfg.maxPlayers);
    if (!g_cfg.host && !g_cfg.hostAddr[0])
        Log("[mp] WARNING: you are a client with no [net] host address. Nothing can be "
            "sent until the host contacts you, which it cannot do first.");
    if (g_cfg.host)
        Log("[mp] race: track=\"%s\" setting=\"%s\" type=%s laps=%d players=%d",
            g_cfg.track, g_cfg.setting, g_cfg.gameType, g_cfg.laps, g_cfg.players);
}

// ---- state ----------------------------------------------------------------
enum Phase { IDLE, LOADING, HELD, RACING };
static Phase        g_phase = IDLE;
// The last RaceSpec we handed to StartRace, so the overlay can show what we
// asked for next to what VuGameConfig actually ended up with.
static RaceSpec     g_askedFor{};
// The hold mode THIS race was called with. g_cfg.holdMode is now editable while
// the game is running, and PreBegin/PostBegin fire during the level load - so
// reading the live setting there let a checkbox ticked mid-load reconfigure a
// race that had already been announced to everyone.
static int          g_raceHoldMode = 0;
// Who we are watching. -1 is our own car. The race keeps simulating either way -
// this only moves the camera, it does not pause or take over anything.
static int          g_spectate = -1;
static uint32_t     g_raceId = 0;
// Where we have actually been, on our own race clock. The victim's half of the
// plausibility gate: given the instant a shot claims to have connected, this says
// where we were then, and the distance from there to here is how far the shooter
// was guessing. ~4 seconds at 30 Hz, which comfortably outlives hitmaxage.
struct PosStamp { float t; float pos[3]; };
static const int kPosRing = 128;
static PosStamp  g_posRing[kPosRing];
static int       g_posAt = 0, g_posUsed = 0;

static void RememberOurPosition(float t, const float pos[3]) {
    g_posRing[g_posAt].t = t;
    memcpy(g_posRing[g_posAt].pos, pos, sizeof(float) * 3);
    g_posAt = (g_posAt + 1) % kPosRing;
    if (g_posUsed < kPosRing) ++g_posUsed;
}
// Nearest sample to t, but only if we really have one. The BOUND matters as much
// as the lookup: nearest-with-no-limit will happily answer a query about a moment
// the ring never covered, and since g_raceTime restarts at 0 every race, an
// unbounded answer can be a position from the previous race on a different track.
// Out of range means "no evidence", and the caller refuses rather than guesses.
static const float kPosTolerance = 0.25f;   // seconds
static bool WhereWeWere(float t, float out[3]) {
    if (g_posUsed <= 0) return false;
    int best = -1; float bestD = 0.f;
    for (int i = 0; i < g_posUsed; ++i) {
        const float d = fabsf(g_posRing[i].t - t);
        if (best < 0 || d < bestD) { best = i; bestD = d; }
    }
    if (best < 0 || bestD > kPosTolerance) return false;
    memcpy(out, g_posRing[best].pos, sizeof(float) * 3);
    return true;
}

// Per-attacker, per-race hit counter. It only has to be unique for as long as a
// claim can still be in flight, so it never needs resetting for correctness -
// but it restarts each race anyway so the log reads sensibly.
static uint32_t     g_hitEventId = 0;
// Per-player, per-race counter for power-up use events.
static uint32_t     g_useEventId = 0;
// Finishing. See WatchOwnFinish / DrainFinishes further down for why these exist.
static bool         g_sentFinish = false;
static uint32_t     g_finishEventId = 0;
static uint32_t     g_finishMs[net::kMaxPeers]{};
static bool         g_haveFinish[net::kMaxPeers]{};
// What our own slots held last GAME TICK - not last packet. A use has to be
// detected at tick rate: the engine's press byte is a one-frame edge and the
// cooldown between uses is 0.1 s, so sampling either at 30 Hz loses them.
struct OwnSlot { const void* pu; int level; int count; };
static OwnSlot      g_ownSlot[3]{};
static bool         g_ownSlotInit = false;
// Refusal-log budgets. Per RACE, not per process: they used to be function-local
// statics that nothing ever reset, so after twenty stale claims anywhere in a
// session the plausibility gate went permanently silent - while the ACCEPTED-hit
// line stayed uncapped, giving the log an accepting-only bias that reads exactly
// like a gate that has stopped refusing anything. The measured drift these lines
// carry is the only data there is for tuning hitmaxdrift.
static int          g_moanAge = 0, g_moanNoHist = 0, g_moanDrift = 0;

// THE RULES IN FORCE FOR THIS RACE, as opposed to what our own ini happens to say.
//
// Latched at the start of the race and not read from g_cfg again, for the same
// reason g_raceHoldMode is latched: a value edited mid-load must not reconfigure a
// race already under way. Whoever CALLS the race sets these - a dedicated server
// from bbr2_server.ini, or the hosting game from its own [net] section - and every
// gate below reads them rather than g_cfg, so the numbers are the same on every
// machine in the session instead of being whatever each player last typed.
//
// g_rulesFromHost records where they came from, purely so the overlay can say.
static net::RuleBody g_rules{};
static bool          g_rulesFromHost = false;

static net::RuleBody RulesFromConfig() {
    net::RuleBody r{};
    r.syncEffects   = g_cfg.syncEffects ? 1 : 0;
    r.syncPowerUps  = g_cfg.syncPowerUps ? 1 : 0;
    r.hitClaims     = g_cfg.hitClaims ? 1 : 0;
    r.effectGrace   = g_cfg.effectGrace;
    r.hitTimeout    = g_cfg.hitTimeout;
    r.hitCooldown   = g_cfg.hitCooldown;
    r.hitMaxAge     = g_cfg.hitMaxAge;
    r.hitMaxDrift   = g_cfg.hitMaxDrift;
    return r;
}
// A rule packet from an older or misconfigured caller must not disable the feature
// by accident, and must not be able to set a value that makes the mod misbehave -
// a zero grace period flickers every hit, a zero timeout strips every claim before
// it can be answered. Clamp rather than reject: a race is more useful than an
// argument about a float.
static void SaneRules(net::RuleBody& r) {
    auto clamp = [](float v, float lo, float hi, float dflt) {
        if (!(v == v) || v <= 0.f) return dflt;      // NaN or nonsense
        return v < lo ? lo : (v > hi ? hi : v);
    };
    r.syncEffects   = r.syncEffects ? 1 : 0;
    r.hitClaims     = r.hitClaims ? 1 : 0;
    r.syncPowerUps  = r.syncPowerUps ? 1 : 0;
    r.effectGrace   = clamp(r.effectGrace,   0.1f,  5.f,  0.6f);
    r.hitTimeout    = clamp(r.hitTimeout,    0.3f, 10.f,  1.5f);
    r.hitCooldown   = clamp(r.hitCooldown,   0.1f, 30.f,  2.0f);
    r.hitMaxAge     = clamp(r.hitMaxAge,     0.1f, 10.f,  1.5f);
    r.hitMaxDrift   = clamp(r.hitMaxDrift,   1.0f, 500.f, 15.0f);
}

// One place, so the host and the client cannot drift apart about what is in force.
static void AdoptRules(const net::RuleBody& r, bool fromHost, const char* who) {
    g_rules = r;
    SaneRules(g_rules);
    g_rulesFromHost = fromHost;
    if (!g_rules.syncEffects) {
        Log("[fx] %s: effect replication is OFF for this race", who);
        return;
    }
    Log("[fx] %s: powerups %s | effects on, claims %s | grace %.1fs timeout %.1fs | "
        "cooldown %.1fs | maxage %.1fs maxdrift %.0fm",
        who, g_rules.syncPowerUps ? "on" : "off", g_rules.hitClaims ? "on" : "off",
        g_rules.effectGrace, g_rules.hitTimeout,
        g_rules.hitCooldown, g_rules.hitMaxAge, g_rules.hitMaxDrift);
}

static uint32_t     g_ranRaceId = 0;    // the race we actually ran...
static uint32_t     g_ranEpoch  = 0;    // ...and which host process called it
static uint32_t     g_myEpoch   = 0;
static bool         g_raceLive = false;
static uint32_t     g_tick = 0;
static double       g_raceTime = 0.0;
static float        g_sendAcc = 0.f;
static float        g_phaseAcc = 0.f;
static bool         g_weOwnRace = false;
static bool         g_autoArmed = false;
static bool         g_holdCountdown = false;
// GO is one unacknowledged datagram; losing it strands a player. Repeat it.
static int          g_goRepeats = 0;
static uint8_t      g_goPhase = 0;
static uint32_t     g_goRaceId = 0;
static float        g_goAcc = 0.f;
static bool         g_goPending = false;
static uint8_t      g_goPendingPhase = 0;
static uint32_t     g_goAppliedRace = 0, g_goAppliedEpoch = 0;

// Per-player, indexed by peer id. Slot g_myId is us and stays empty here.
struct RemotePlayer {
    bool          inThisRace = false;   // was this player live when the grid was built?
    Vehicle       cfg{};
    VehicleEntity ent{};
    RemoteSync    sync{};
    uint32_t      appliedTick = 0;
    bool          labelled = false;
    char          shownName[96]{};
    bool          announcedReady = false;
    // Who this grid slot belongs to, captured when the grid was built. Both
    // departure paths ZERO the Peer record before we notice they left - the host
    // on BYE, clients on the next LOBBY that omits them - so by the time the
    // leave edge fires, p->name is already empty. Keep our own copy.
    char          ownerName[24]{};
    uint32_t      ownerHash = 0;
    // They stopped leaving the session but stopped sending: quit to the menu, or
    // a stall. Seconds since their last NEW state packet.
    float         stateAge = 0.f;
    // Throttles re-asking for an effect the engine keeps refusing.
    float         applyAcc = 0.f;
    // An effect that is on their car but NOT in their latest report is not
    // necessarily a phantom - it may simply be one our own simulation landed
    // first, with their confirmation still in flight. So each unreported effect
    // gets a grace period before we take it off. See the mirror block.
    // `claimed` marks one we have asserted to the victim under shooter
    // authority: it gets hitTimeout to be answered rather than the ordinary
    // effectGrace, because a claim has a round trip AND a retransmit window to
    // survive before silence means "refused".
    struct Unconfirmed { uint32_t hash; float age; bool claimed; };
    Unconfirmed   pending[8]{};
    int           pendingCount = 0;
    // What was on their car LAST frame. A hash that is here now and was not a
    // frame ago is the moment our own simulation landed something - which is the
    // only hit signal we have, since we do not own the projectile. Edge-detecting
    // matters: testing "is it on the car" instead would re-claim every frame and
    // re-freeze the victim forever.
    uint32_t      ghost[24]{};
    int           ghostCount = 0;
    bool          ghostInit  = false;   // first frame just adopts, never claims
    // Backstop against any claim loop the fire window does not catch: the same
    // effect is never claimed against the same player twice inside hitCooldown.
    struct ClaimCool { uint32_t hash; float left; };
    ClaimCool     cool[8]{};
    int           coolCount = 0;
    // How many claims we have already sent against this player for each effect
    // this race. A hard cap makes any claim loop TERMINATE, which the cooldown
    // only slows down. Twelve freezes of the same player with the same weapon in
    // one race is past anything real; a runaway loop reaches it in half a minute
    // and then stops for good.
    struct ClaimTally { uint32_t hash; uint16_t sent; bool moaned; };
    ClaimTally    tally[12]{};
    int           tallyCount = 0;
    bool          tallyFull = false;
    bool          effectsStripped = false;   // wreck already cleaned up
    bool          gone = false;         // left; the car is a wreck now
};
static RemotePlayer g_rp[net::kMaxPeers];
// "The car in this grid slot belongs to somebody who left mid-race." The car
// itself has to stay - it is one of VuBaseGame's entities and the results table,
// the position logic and the HUD all reference it - but nothing may drive it any
// more. Indexed by slot and cleared per race, NOT by peer id liveness: the slot
// can be handed to a newcomer while the abandoned car is still sitting there.
static bool g_ghostCar[net::kMaxPeers] = {};

// Cheap identity for a grid slot. Slots are recycled - AssignSlot hands a
// departed player's id to the next person who connects - so "is this still the
// same human?" has to be asked before re-adopting a car we already gave up on.
// The nameplate strings carry the engine's inline color escape, "{[r,g,b]}".
// The game renders it; ImGui would print it verbatim, so strip it on the way in.
static void StripColor(char* dst, size_t cap, const char* src) {
    size_t o = 0;
    for (const char* p = src; p && *p && o + 1 < cap; ) {
        if (p[0] == '{' && p[1] == '[') {
            const char* close = strstr(p, "]}");
            if (close) { p = close + 2; continue; }
        }
        dst[o++] = *p++;
    }
    // Trim the space the tag was separated by, if it is now trailing.
    while (o && dst[o-1] == ' ') --o;
    dst[o] = 0;
}

static uint32_t NameHash(const char* n) {
    uint32_t h = 2166136261u;
    for (; n && *n; ++n) h = (h ^ (uint8_t)*n) * 16777619u;
    return h ? h : 1u;
}

static int MyId() { return net::MyId(); }
static bool HaveId() { return net::MyId() != net::kUnassignedId; }

// ---- toasts ---------------------------------------------------------------
static float g_notifyAge = 1e9f;
static char  g_notifyLast[128] = "";
static void NotifyTick(float dt) { g_notifyAge += dt; }

static void Notify(const char* body) {
    if (!g_cfg.popups) { Log("[ui] (popups off) %s: %s", g_cfg.toastTag, body); return; }
    // Swallow a repeat of the same message inside a few seconds. Several paths
    // legitimately want to say "waiting..." and repeating it adds nothing.
    // Still 10s rather than 3, even now that a new toast closes the old one:
    // the guard is about not saying the same thing twice, not about spacing.
    if (g_notifyAge < 10.0f && strcmp(g_notifyLast, body) == 0) return;
    g_notifyAge = 0.f;
    strncpy_s(g_notifyLast, body, sizeof(g_notifyLast) - 1);
    ShowToast(g_cfg.toastTag, body);
}
static void ClearNotify() {}

// Who this player should be waiting on, in words. On a dedicated server there is
// no host, so "the host" sends them looking for a player who does not exist.
static const char* StarterName() {
    return (net::ServerFlags() & net::SF_HEADLESS) ? "an admin" : "the host";
}
// Can WE start it? True for a game host, and for an admin on a dedicated server.
static bool WeStartRaces() { return net::IsAdmin(); }

static net::Identity MyIdentity() {
    net::Identity id;
    id.name = g_cfg.name;       id.vehicle = g_cfg.vehicle;
    id.driver = g_cfg.driver;   id.paint = g_cfg.paint;
    id.paintDetail = g_cfg.paintDetail; id.decal = g_cfg.decal;
    return id;
}
static void Hello() { net::SendHello(MyIdentity(), 0, g_cfg.seed); }

// ---- lobby queries --------------------------------------------------------
static int LobbyReady()  { return net::ReadyCount(g_raceId); }
// We are ready when our own race is loaded and sitting on a hold. Without
// telling the transport, ReadyCount can never reach the player count and every
// "is everyone ready" test is permanently off by one.
static void UpdateSelfReady() {
    const bool holding = LoadGateHeld() ||
                         (g_raceLive && (StartGateHeld() || PreGameHeld() || CountdownHeld()));
    net::SetSelfReady(holding && g_raceId != 0, g_raceId);
}
static int LobbySize()   { return net::PlayerCount(); }

// Counts always say "you + N others". "3 players connected" left you working out
// whether you were one of the three.
static void Others(char* out, size_t cap, int others) {
    if (others == 1) strncpy_s(out, cap, "1 other player", cap - 1);
    else             sprintf_s(out, cap, "%d other players", others);
}

// Shorter form for the join/leave line, which already names the player.
static void OthersShort(char* out, size_t cap, int others) {
    if (others == 1) strncpy_s(out, cap, "1 other", cap - 1);
    else             sprintf_s(out, cap, "%d others", others);
}

static void StatusToast() {
    char body[160], who[64];
    const int total = LobbySize();
    const int others = total - 1;
    const int ready = LobbyReady();
    if (others <= 0)      sprintf_s(body, "Just you so far - nobody else has connected");
    else {
        Others(who, sizeof(who), others);
        if (g_raceLive)   sprintf_s(body, "Racing: you + %s", who);
        else              sprintf_s(body, "You + %s, %d of %d ready", who, ready, total);
    }
    Notify(body);
    Log("[mp] status: %s", body);
}

// ---- hotkeys ---------------------------------------------------------------
static bool KeyPressed(int vk) {
    static bool down[256] = {};
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = now && !down[vk & 0xff];
    down[vk & 0xff] = now;
    return edge;
}

static void CallRace(bool holdAtLoad);      // fwd
static void RequestOrBeginRace(const char* who);   // fwd - needs BuildRaceBody
static void RequestOrDropLights(const char* who);  // fwd

// The single entry point for "start the race described by the ini". F9 and
// autostart both come through here.
static void BeginRace(const char* who) {
    // A race that is already being SET UP is not "live" yet - g_raceLive only
    // goes true in PostBegin, which is a whole level load later. Without this,
    // a second F9 (or a second click) during the load called the whole race
    // again underneath the one in flight: new raceId, new RACE broadcast,
    // clients dropping a half-loaded level to load another one.
    if (g_phase == LOADING || g_phase == HELD) {
        if (g_phaseAcc < 60.f) {
            Log("[mp] %s ignored - a race is already being set up (%.0fs in)",
                who, g_phaseAcc);
            return;
        }
        Log("[mp] %s: the previous race has been loading for %.0fs - assuming it "
            "failed and calling a new one", who, g_phaseAcc);
    }
    g_raceHoldMode = g_cfg.holdMode;
    const bool holdLoad = (g_raceHoldMode & 1) != 0;
    const bool holdGrid = (g_raceHoldMode & 6) != 0;
    Log("[mp] %s: calling race  loadHold=%d gridHold=%d players=%d",
        who, (int)holdLoad, (int)holdGrid, LobbySize());
    net::ResetRaceState();
    // Everyone is loading until they say otherwise - this is what drives the
    // "[loading...]" label on their car.
    net::SetAllFlags(net::PF_LOADING, net::PF_READY | net::PF_RACING);
    if (g_raceHoldMode & 4) { g_holdCountdown = true; if (g_cfg.doCountdown) HoldCountdown(); }
    g_goPending = false; g_goRepeats = 0;
    g_goAppliedRace = 0; g_goAppliedEpoch = 0;
    if (holdGrid) HoldStartGate();
    g_weOwnRace = true;
    CallRace(holdLoad);
    if (holdLoad) {
        Log("[mp] holding on the LOADING SCREEN. F10 to go, or wait for everyone.");
        Notify(LobbySize() > 1 ? "Waiting for players to load..."
                               : "Waiting - press F10 to start the race");
    }
}

static void LogStatus() {
    RaceSpec cur{}; GetCurrentRace(cur);
    Log("---- status ----------------------------------------------");
    Log("  role=%s  myId=%d  players=%d (ready %d)  phase=%d  raceLive=%d  race=%u",
        net::IsHost() ? "HOST" : "client", MyId(), LobbySize(), LobbyReady(),
        (int)g_phase, (int)g_raceLive, g_raceId);
    for (int i = 0; i < net::kMaxPeers; ++i) {
        net::Peer* p = net::GetPeer(i);
        if (!p || !net::PeerAlive(i)) continue;
        Log("  slot %2d: \"%s\"%s  ready=%d flags=%02x vehicle=\"%s\" lastTick=%u",
            i, p->name, i == MyId() ? " (you)" : "", (int)p->ready, p->flags,
            p->vehicle, p->lastTick);
    }
    Log("  countdown: suppressed=%d  preGameFrozen=%d  startGate=%s",
        (int)CountdownHeld(), (int)PreGameHeld(), StartGateHeld() ? "HELD" : "open");
    Log("  live race: track=\"%s\" setting=\"%s\" type=%d laps=%d",
        cur.track, cur.setting, cur.gameType, cur.lapCount);
    net::NetStats ns{}; net::GetStats(ns);
    Log("  net: tx=%u  rx=%u (good %u, junk %u, wrong-version %u, own-id %u) relayed=%u",
        ns.txTotal, ns.rxTotal, ns.rxGood, ns.rxJunk, ns.rxBadVersion, ns.rxSelf, ns.relayed);
    Log("  hit claims: sent=%u recvd=%u dupes=%u relayed=%u refused=%u "
        "undeliverable=%u repeats-dropped=%u",
        ns.hitsSent, ns.hitsRecv, ns.hitsDupes, ns.hitsRelayed, ns.hitsRefused,
        ns.hitsUndeliverable, ns.hitsDropped);
    if (!net::IsHost()) Log("  net: host at %s", ns.hostAddr[0] ? ns.hostAddr : "UNKNOWN");
    if (ns.rxTotal == 0) {
        Log("  net: NOTHING has arrived on UDP %d. In order of likelihood:", g_cfg.port);
        Log("       0. OVER THE INTERNET: plain UDP does not traverse NAT on its own.");
        Log("          The HOST needs UDP %d forwarded, and clients need the host's", g_cfg.port);
        Log("          PUBLIC address - not a LAN 192.168.x.x, and not an address a");
        Log("          website reported while a VPN or Cloudflare WARP was on, because");
        Log("          that one belongs to the VPN and nothing can route back through");
        Log("          it. Tailscale or ZeroTier sidesteps all of this.");
        Log("       1. Windows Firewall is blocking Game_x64.exe. Allow it, or run:");
        Log("          netsh advfirewall firewall add rule name=\"BBR2MP\" dir=in "
            "action=allow protocol=UDP localport=%d", g_cfg.port);
        Log("       2. Nobody has the host's address in [net] host.");
    }
    Log("  pause: refcount=%d  freeze-bypass=%s", PauseRefCount(),
        PauseBypass() ? "on (world keeps running)" : "off");
    DumpPopupState("F8");
    Log("  F9 call race   F10/F11 GO   F7 lobby status   F8 this dump");
    Log("----------------------------------------------------------");
}

static void SendGoReliably(uint8_t phase) {
    if (phase == 1) net::SetAllFlags(net::PF_RACING, net::PF_LOADING);
    // A level load easily outlasts a couple of seconds and a client that is still
    // loading cannot act on a GO yet. GO is idempotent, so over-sending is free.
    g_goPhase = phase; g_goRepeats = 60; g_goAcc = 0.f;
    g_goRaceId = g_raceId;
    net::SendGo(g_goRaceId, g_myEpoch, phase);
}

// True when something is actually holding a race back. Nothing here is a GO
// unless one of these is set; without the test, a GO during a running race with
// all gates open fell through to FireStartGate() and re-fired the start
// transition - on every machine, because the GO is broadcast.
static bool SomethingIsHolding() {
    return LoadGateHeld() || StartGateHeld() || PreGameHeld() || CountdownHeld();
}

static void DropTheLights(const char* who) {
    if (g_raceId == 0) {
        Log("[mp] %s ignored - no race has been called yet (press F9 first)", who);
        return;
    }
    if (g_raceLive && !SomethingIsHolding()) {
        Log("[mp] %s ignored - the race is already running and nothing is holding it", who);
        return;
    }
    if (!g_raceLive && !LoadGateHeld()) {
        if (g_phase != LOADING && g_phase != HELD) {
            Log("[mp] %s ignored - not loading a race right now (press F9 first)", who);
            return;
        }
        SendGoReliably(1);
        g_goPending = true; g_goPendingPhase = 1;
        Log("[mp] %s: players told to go; our own race is still loading - deferred", who);
        return;
    }
    if (LoadGateHeld()) {
        SendGoReliably(0);
        ReleaseLoadGate();
        g_phase = HELD;
        Log("[mp] %s: load gate released - loading now", who);
        return;
    }
    SendGoReliably(1);
    if (PreGameHeld() || CountdownHeld()) {
        // Release BOTH holds, then fire the timeline from the top so players get a
        // real 3-2-1. ReleaseStartGate must happen before StartCountdown: the
        // graph's last step IS the engine's own start, and with the gate stubbed
        // that step would be swallowed.
        g_holdCountdown = false;
        ReleasePreGame();
        ReleaseStartGate();
        if (g_cfg.doCountdown) StartCountdown();
        Notify("Get ready!");
        Log("[mp] %s: holds released, countdown running", who);
        return;
    }
    if (FireStartGate()) Notify("Go!");
    Log("[mp] %s: lights out", who);
}

static void PollHotkeys() {
    // These are read with GetAsyncKeyState, which never passes through the window
    // procedure the overlay filters - so the overlay cannot swallow them and has
    // to be asked instead. Matters the moment the panel grows a text box.
    //
    // Poll every key first and only THEN decide whether to act. Returning early
    // would leave KeyPressed's edge state stale, so a key held down while typing
    // in the panel would fire the moment you clicked away - a phantom F9.
    const bool f7  = KeyPressed(VK_F7);
    const bool f8  = KeyPressed(VK_F8);
    const bool f9  = KeyPressed(VK_F9);
    const bool f10 = KeyPressed(VK_F10);
    const bool f11 = KeyPressed(VK_F11);
    if (gui::WantsKeyboard()) return;

    if (f7) StatusToast();
    if (f8) LogStatus();

    if (f9) {
        if (!net::IsAdmin()) {
            Log("[mp] F9 IGNORED - this machine is not the host, and the server does");
            Log("[mp]   not list you as an admin. You will be told what to load.");
            char msg[96];
            sprintf_s(msg, "%s starts the race - you will be pulled in", StarterName());
            Notify(msg);
        }
        else if (g_raceLive)      Log("[mp] F9 ignored - a race is already running (finish or quit it first)");
        else if (net::IsHost() && !g_cfg.track[0])
                                  Log("[mp] F9 ignored - [race] track is blank in bbr2_mp.ini");
        else                      RequestOrBeginRace("F9");
    }

    if (f10) RequestOrDropLights("F10");
    if (f11) RequestOrDropLights("F11");
}

// ---- roster -> config -----------------------------------------------------
static void DressVehicle(Vehicle v, const char* veh, const char* drv,
                         const char* paint, const char* detail, const char* decal) {
    // The name is only consulted if the Customization has no "Alternate" entry -
    // that is checked first and wins, and the game leaves one behind from whatever
    // car was last picked in its own menus. Drop it, or the name is decoration.
    if (veh && veh[0] && g_cfg.doClearAlt) ClearVehicleAlternate(v);
    if (veh    && veh[0])    SetVehicleString(v, 0, veh);
    if (drv    && drv[0])    SetVehicleString(v, 1, drv);
    if (paint  && paint[0])  SetVehicleString(v, 3, paint);
    if (detail && detail[0]) SetVehicleString(v, 4, detail);
    if (decal  && decal[0])  SetVehicleString(v, 5, decal);
}

// Move the local player to the config slot matching our player id, so grid
// position (index % spawnPoints) agrees on every machine. prepareQuick always
// puts the human at index 0; if we are not player 0 we swap the control fields
// with whichever AI is sitting in our slot.
// EVERY early return says why. This is the one function whose silent failure
// looks exactly like "everybody spawned in the same place": the engine seats car
// `i` at spawn point `i % spawnPointCount` at level init, so two cars at different
// config indices CANNOT share a spawn. If two players end up on top of each other
// it is because both machines left their human at the same index - and until now
// two of the four ways that happens produced no log line at all.
static void TakeMySlot(int myId) {
    if (!g_cfg.gridSlots) {
        Log("[mp] grid slots are OFF ([race] gridslots) - staying at config index 0. "
            "If more than one player does this, everyone shares a spawn point.");
        Notify("Grid slot NOT taken: gridslots is off in bbr2_mp.ini");
        return;
    }
    if (myId <= 0) {
        // Silent until now, and one of the two ways this goes wrong: ApplyRoster
        // falls back to 0 when we have no id yet, which is indistinguishable from
        // legitimately BEING player 0. Say which one it is.
        if (!HaveId()) {
            Log("[mp] no player id yet - racing from config index 0. If the host is "
                "also at index 0 we share a spawn point.");
            Notify("Grid slot NOT taken: no player id yet");
        }
        return;
    }
    if (myId >= VehicleCount()) {
        Log("[mp] WARNING: we are player %d but the grid only has %d cars - staying "
            "in slot 0, which means sharing a spawn point. Raise [race] players.",
            myId, VehicleCount());
        { char m[96]; sprintf_s(m, "Grid slot NOT taken: only %d cars for player %d",
                                  VehicleCount(), myId); Notify(m); }
        return;
    }
    Vehicle zero = VehicleAt(0), mine = VehicleAt(myId);
    if (!zero || !mine) {
        Log("[mp] CANNOT take grid slot %d: config slot %d is %s and slot 0 is %s - "
            "staying at index 0 and sharing a spawn point",
            myId, myId, mine ? "there" : "MISSING", zero ? "there" : "MISSING");
        Notify("Grid slot NOT taken: a config slot is missing");
        return;
    }
    // MUST be idempotent: this runs from the race-config callback AND again from
    // the begin hook, with no prepare* in between to undo it. Running it twice
    // used to read the (now AI) slot 0's empty mask and -1 viewport and stamp
    // those onto us - leaving nobody with a controller and the camera with no
    // target, i.e. a car that cannot be steered.
    if (!mine.isAi()) {
        // Either we already swapped (normal - this runs twice per race), or the
        // config genuinely has a second human at that index, in which case taking
        // it would leave two cars fighting over one controller. Distinguish them:
        // after a successful swap WE are the local player at this index.
        static int said = 0;
        if (!VehicleIsLocalPlayer(mine) && said++ < 4) {
            Log("[mp] CANNOT take grid slot %d: it is already a NON-AI car that is "
                "not us. Staying at index 0, which means sharing a spawn point with "
                "whoever is there.", myId);
            Notify("Grid slot NOT taken: that car is already a human");
        }
        return;
    }
    const uint32_t mask = zero.controllerMask();
    const int      vp   = zero.viewportIndex();
    mine.setAi(false); mine.setControllerMask(mask); mine.setViewportIndex(vp);
    SetVehicleLocalPlayer(mine, true);
    zero.setAi(true);  zero.setControllerMask(0);    zero.setViewportIndex(-1);
    SetVehicleLocalPlayer(zero, false);
    Log("[mp] took grid slot %d (swapped with the AI that was there)", myId);
}

// Runs inside StartRace, after prepareQuick has rebuilt the config and before the
// level load starts. This is the ONLY point at which vehicle and driver models
// are still up for grabs - the load resolves them from these strings.
static void FixGridOrder();   // defined below, with the rest of the grid code

static void ApplyRoster(void* = nullptr) {
    const int n = VehicleCount();
    if (n < 1) { Log("[mp] roster: no vehicles in the config yet - nothing to do"); return; }
    FixGridOrder();
    const int me = HaveId() ? MyId() : 0;
    // The three inputs that decide whether the grid-slot swap can run at all.
    // Without them a failed swap is indistinguishable from a swap that was never
    // needed, which is how this went unnoticed: the HOST is player 0 and returns
    // at the first guard every time, so the swap has only ever run on a client.
    Log("[mp] roster: %d cars, we are player %d (id assigned: %s, gridslots: %s)",
        n, me, HaveId() ? "yes" : "NO", g_cfg.gridSlots ? "on" : "OFF");
    TakeMySlot(me);

    Vehicle mine = VehicleAt(me < n ? me : 0);
    if (mine) {
        DressVehicle(mine, g_cfg.vehicle, g_cfg.driver, g_cfg.paint,
                     g_cfg.paintDetail, g_cfg.decal);
        SetVehicleName(mine, g_cfg.name);
        Log("[mp] slot %d is us: vehicle=\"%s\" driver=\"%s\"",
            me, GetVehicleString(mine,0), GetVehicleString(mine,1));
    }

    for (int i = 0; i < net::kMaxPeers && i < n; ++i) {
        if (i == me) continue;
        net::Peer* p = net::GetPeer(i);
        if (!p || !net::PeerAlive(i) || !p->helloSeen) continue;
        Vehicle v = VehicleAt(i);
        if (!v) continue;
        DressVehicle(v, p->vehicle, p->driver, p->paint, p->paintDetail, p->decal);
        SetVehicleName(v, p->name);
        Log("[mp] slot %d is \"%s\": vehicle=\"%s\" driver=\"%s\"",
            i, p->name, GetVehicleString(v,0), GetVehicleString(v,1));
    }
}

// ---- race lifecycle -------------------------------------------------------
// Seconds per bubble reroll window.
//
// This is a straight trade and it is worth being explicit about it. A bubble's
// type is a function of the window it respawns in, so:
//   too LONG  - collect the same bubble twice inside one window and it comes back
//               as exactly what it was. Bubbles respawn in about a second, so 5s
//               meant a player circling one pickup saw the same type every time.
//   too SHORT - the window number is published by the lowest live player and
//               reaches everyone else about 50-80ms late, so a respawn landing
//               near a boundary can be typed from different windows on different
//               machines. The chance of that is roughly (staleness / window).
// 2s puts consecutive respawns in different windows almost always, at a few
// percent chance of one bubble looking different on the two screens for a single
// cycle - and the next respawn puts it back in step, which a per-bubble respawn
// counter would not: that would diverge the first time one machine registered a
// grazing pickup the other missed, and stay diverged for the rest of the race.
static const float kBubbleEpochSec = 2.0f;

static void ResetPlayers() {
    for (int i = 0; i < net::kMaxPeers; ++i) {
        g_rp[i] = RemotePlayer{};
        g_rp[i].sync.delayScale = g_cfg.interpDelay;
        g_ghostCar[i] = false;
    }
}

static void PreBegin(void*) {
    // The pause menu's Restart re-runs begin() without going through F9.
    if (g_weOwnRace && (g_raceHoldMode & 6)) HoldStartGate();
    SetBubbleSeed(g_cfg.seed); SeedRng(g_cfg.seed);
    ApplyRoster();
    ResetPlayers();
    // Per-race, and reset HERE as well as in PreEnd: a pause-menu Restart re-runs
    // begin() without end(), and a stale g_sentFinish means we never tell anyone we
    // finished - reproducing the exact bug this exists to fix, on the path most
    // likely to be used while testing it.
    g_sentFinish = false;
    g_useEventId = 0;
    g_ownSlotInit = false;
    for (int i = 0; i < net::kMaxPeers; ++i) { g_haveFinish[i] = false; g_finishMs[i] = 0; }
    g_tick = 0; g_raceTime = 0.0;
    g_posAt = g_posUsed = 0;
    net::ResetHits();
    ResetCountdownState();
    const int me = HaveId() ? MyId() : 0;
    // Snapshot who is actually IN this race. Someone who connects later gets the
    // next free id, and without this we would install remote input on whatever AI
    // is sitting in that grid slot - a live opponent that then coasts to a stop,
    // or gets teleported onto a newcomer's synthetic path.
    for (int i = 0; i < net::kMaxPeers && i < VehicleCount(); ++i) {
        if (i == me) continue;
        if (!net::PeerAlive(i)) continue;
        net::Peer* p = net::GetPeer(i);
        if (!p || !p->helloSeen) continue;
        g_rp[i].cfg = VehicleAt(i);
        g_rp[i].inThisRace = true;
        strncpy_s(g_rp[i].ownerName, p->name, sizeof(g_rp[i].ownerName)-1);
        g_rp[i].ownerHash = NameHash(p->name);
    }
    Log("[mp] pre-begin: roster=%d, we are slot %d", VehicleCount(), me);
}

// Counts up from 0 once a race goes live, so the grid dump can be repeated after
// the cars have settled; -1 means "already done for this race".
static float g_gridReportAt = -1.f;

// ---- the starting-order mode ----------------------------------------------
// The one that actually mattered, and the reason three earlier attempts at this
// did nothing. Level init does not place config vehicle i at spawn point i: at
// 0x140177f5a it copies the entity vector, REORDERS the copy according to
// VuGameConfig+0x14, and places the i-th entry of the copy at spawn (i % n).
// prepareQuick sets that field to 4 - "partition by the isAI flag", i.e. put the
// human at the back of the grid. Single player wants exactly that. Two machines
// cannot survive it, because "the human" is a different car on each one, so both
// machines move their own player to the same end and the two cars claim the same
// spawn point. It also explains the 8-car race: our car sat at config index 1 and
// was placed at spawn 7, with every AI shifted up one.
//
// Setting it to 0 makes the grid follow the config order, which is the one thing
// both machines already agree on - that is what TakeMySlot has been maintaining
// all along. [race] gridfix = false leaves the engine's own mode alone.
static void FixGridOrder() {
    if (!g_cfg.gridFix || !net::Ready() || !HaveId()) return;
    const int mode = GridOrderMode();
    if (mode == 0) return;
    SetGridOrderMode(0);
    static int said = 0;
    if (said++ < 3)
        Log("[grid] starting-order mode was %d (%s) - set to 0 so the grid follows "
            "the config order, which every machine agrees on", mode,
            mode == 1 ? "reversed"      : mode == 2 ? "shuffled" :
            mode == 3 ? "sorted+reversed" : mode == 4 ? "human moved to the back" :
            mode == 5 ? "sorted, human at the back" : "unknown");
}

// ---- grid diagnostics ------------------------------------------------------
// "we all spawn in the same spot" has exactly one mechanism behind it. The engine
// seats config vehicle i at spawn point (i % spawnPointCount) once, at level init
// (0x177970), with no clamp and no per-car offset - so two cars at DIFFERENT
// config indices CANNOT share a spawn point. If two players overlap, both
// machines have left their human on the same config index; the client then
// reports that index-0 position over the wire and its ghost parks on top of the
// host's car.
//
// This dump settles it from one race, from either machine's log. It deliberately
// does NOT trust EntityAt(i) to line up with VehicleAt(i) - it resolves each
// entity back to its own config vehicle and prints both numbers, because if that
// mapping is not the identity then the swap in TakeMySlot is aimed at the wrong
// car and everything downstream of it is wrong too.
static void GridReport(const char* when) {
    const int nEnt = EntityCount(), nCfg = VehicleCount();
    VehicleEntity me = LocalPlayerEntity();
    Log("[grid] %s: %d config vehicles, %d entities, our player id is %d",
        when, nCfg, nEnt, HaveId() ? MyId() : -1);
    // The spawn-point list, in the order the ENGINE holds it - which is the order
    // that decides who starts where. Both machines print this; if the two lists
    // are in different orders, then "config index 1" means a different patch of
    // tarmac on each machine and the cars are placed correctly but not in the
    // same places. The path hash is the same number on every machine for the same
    // spawn point, so the two logs can be lined up directly.
    {
        SpawnPoint sp[16];
        const int ns = ReadSpawnPoints(sp, 16);
        if (ns <= 0) Log("[grid]   spawn points: NONE READABLE");
        else for (int i = 0; i < ns; ++i)
            Log("[grid]   spawn %2d (engine order): pos=(%.2f, %.2f, %.2f) path=%016llx",
                i, sp[i].pos[0], sp[i].pos[1], sp[i].pos[2], sp[i].pathHash);
    }
    float px[32], py[32], pz[32]; bool ok[32] = {};
    for (int i = 0; i < nEnt && i < 32; ++i) {
        VehicleEntity e = EntityAt(i);
        if (!e) { Log("[grid]   entity %2d: <null>", i); continue; }
        int cfgIdx = -1;
        Vehicle ec = e.config();
        for (int j = 0; j < nCfg; ++j) if (VehicleAt(j).p == ec.p) { cfgIdx = j; break; }
        VehicleState st{};
        ok[i] = CaptureState(e, st);
        if (ok[i]) { px[i] = st.pos[0]; py[i] = st.pos[1]; pz[i] = st.pos[2]; }
        const char* who = (e.p == me.p) ? "US    "
                        : (cfgIdx >= 0 && cfgIdx < net::kMaxPeers && g_rp[cfgIdx].inThisRace)
                          ? "REMOTE" : "ai    ";
        if (ok[i])
            Log("[grid]   entity %2d -> config %2d  %s  ai=%d local=%d  \"%s\"  "
                "pos=(%.2f, %.2f, %.2f)",
                i, cfgIdx, who, ec ? (int)ec.isAi() : -1,
                ec ? (int)VehicleIsLocalPlayer(ec) : -1,
                ec ? GetVehicleName(ec) : "?", px[i], py[i], pz[i]);
        else
            Log("[grid]   entity %2d -> config %2d  %s  ai=%d local=%d  \"%s\"  "
                "pos=<could not read>",
                i, cfgIdx, who, ec ? (int)ec.isAi() : -1,
                ec ? (int)VehicleIsLocalPlayer(ec) : -1,
                ec ? GetVehicleName(ec) : "?");
    }
    // Say the conclusion out loud rather than leaving it to be eyeballed. Two
    // cars within 3 m at the start line are interpenetrating, which is the
    // reported symptom.
    for (int a = 0; a < nEnt && a < 32; ++a) {
        if (!ok[a]) continue;
        for (int b = a + 1; b < nEnt && b < 32; ++b) {
            if (!ok[b]) continue;
            const float dx = px[a]-px[b], dy = py[a]-py[b], dz = pz[a]-pz[b];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < 9.0f)
                Log("[grid]   !! entities %d and %d are %.2f m apart - SHARING a spot",
                    a, b, sqrtf(d2));
        }
    }
}

static void PostBegin(void*) {
    RaceSpec chk{}; GetCurrentRace(chk);
    // HOST ONLY: a client races whatever the host sent, which is routinely not
    // what its own [race] track says.
    if (net::IsHost() && g_weOwnRace && g_cfg.track[0] && _stricmp(chk.track, g_cfg.track) != 0) {
        g_weOwnRace = false;
        ReleaseStartGate();
    }
    const bool startedByUs = (g_phase == LOADING || g_phase == HELD) || g_weOwnRace;
    g_raceLive = true;
    g_phase = RACING;
    // ONLY when there is a race id to record. A race started from the game's own
    // menus has none - PreEnd zeroes it on a client - and writing that 0 here
    // disarmed the "already ran it" test below, which is guarded on
    // `g_ranRaceId != 0`. One menu race was therefore enough to make a stale
    // latched RACE packet acceptable again, and the next time the player quit to
    // the menu the mod loaded a race from several minutes earlier straight into a
    // game that was tearing down.
    if (g_raceId != 0) g_ranRaceId = g_raceId;
    RaceSpec cur{}; GetCurrentRace(cur);
    Log("[mp] race live: track=\"%s\" setting=\"%s\" laps=%d entities=%d",
        cur.track, cur.setting, cur.lapCount, EntityCount());

    if (g_cfg.skipIntro) SkipIntro(false);

    GridReport("grid at level init");
    g_gridReportAt = 0.f;   // and again once the cars have settled, see OnTick

    if (!startedByUs) {
        Log("[mp] (this race was started from the MENU, not by the mod)");
        // Nobody called it, so nobody set the rules. Ours are the only ones there
        // are - and leaving the previous race's in force would be worse.
        AdoptRules(RulesFromConfig(), false, "menu race, local rules");
        ReleaseStartGate();
    } else {
        if (g_raceHoldMode & 4) {
            HoldPreGame();
            g_holdCountdown = true;
            if (g_cfg.doCountdown) HoldCountdown();
            Log("[mp] cars parked on the grid, countdown suppressed - F10 to start it");
        }
        if (StartGateHeld() || PreGameHeld() || CountdownHeld()) {
            // WeStartRaces(), not IsHost(): on a dedicated server the admin sitting
            // at this hold is the one who presses F10, and telling them to wait for
            // a host they do not have is how you get a lobby staring at each other.
            char msg[96];
            if (WeStartRaces()) strcpy_s(msg, "Waiting for players - press F10 to start");
            else sprintf_s(msg, "Waiting for %s to start the race", StarterName());
            Notify(msg);
        }
    }
    Hello();
}

// Registered with atexit so a normal quit tells everyone immediately, instead of
// leaving them to notice after the 8-second timeout - long enough for the host to
// call a race that sizes the grid for, and waits on, someone already gone.
static void OnExit() { gui::Shutdown(); net::SendBye(); net::Shutdown(); }

// Seconds since the game's end() hook last fired. A race command acted on inside
// that window starts a level load while the engine is still unwinding the last
// game object and returning to the menu - which is how you get
// "Asset '' of type VuProjectAsset does not exist!" and a crash.
static float        g_endAcc = 1e6f;

static void PreEnd(void*) {
    ClearNotify();
    g_endAcc = 0.f;
    g_spectate = -1;        // the cars are about to be freed
    // Nothing from this race may leak into the next one's grid. HoldPreGame
    // byte-patches a vtable slot, and leaving it patched would park the NEXT race
    // - even one started from the game's own menus.
    ReleasePreGame();
    ReleaseCountdown();
    ReleaseStartGate();
    ReleaseLoadGate();
    g_goRepeats = 0; g_goPending = false;
    g_goAppliedRace = 0; g_goAppliedEpoch = 0;
    g_autoArmed = false;
    g_holdCountdown = false;
    ResetCountdownState();
    g_raceLive = false;
    g_gridReportAt = -1.f;
    g_phase = IDLE;
    // Nothing about a hit survives a race. An effect bitmask leaking across the
    // boundary is precisely the class of bug that took three rounds to shake out
    // of the grid-slot code, and a claim is the same shape.
    net::ResetHits();
    g_hitEventId = 0;
    g_useEventId = 0;
    g_ownSlotInit = false;
    g_sentFinish = false;
    for (int i = 0; i < net::kMaxPeers; ++i) { g_haveFinish[i] = false; g_finishMs[i] = 0; }
    g_moanAge = g_moanNoHist = g_moanDrift = 0;
    // The position history is about US, so ResetPlayers does not touch it - and it
    // leaks across a race boundary if nothing else does. g_raceTime restarts at 0
    // every race, so the old ring's timestamps collide with the new race's, on a
    // different track, and the plausibility gate would measure drift against a
    // position from a world that no longer exists.
    g_posAt = g_posUsed = 0;
    ResetPlayers();
    // Clients forget the race id so a straggling GO cannot match; the host keeps
    // its monotonic counter.
    if (!net::IsHost()) { g_raceId = 0; g_weOwnRace = false; }
}

// ---- remote car input -----------------------------------------------------
static void RemoteDrive(VehicleEntity v, float, void* user) {
    const int id = (int)(intptr_t)user;
    net::Peer* p = net::GetPeer(id);

    // Returning without writing is NOT the same as writing nothing: the engine's
    // input object keeps whatever we put there last. A player who quit while
    // holding the throttle would leave their car driving itself into the scenery
    // for the rest of the race. Write zeroes instead and let it coast to a stop.
    //
    // g_ghostCar, not PeerAlive: the slot can be reassigned to somebody who joins
    // during the race, and their steering must not reach the car the previous
    // occupant abandoned.
    if (g_ghostCar[id & (net::kMaxPeers - 1)] || !p || !p->haveState) {
        ApplyInputs(v, VehicleState{});
        return;
    }
    VehicleState st{};
    st.steer = p->state.steer; st.throttle = p->state.throttle;
    // MASK OFF THE THREE FIRE BUTTONS (UsePowerUp1, UsePowerUp2, UseAbility).
    //
    // Replaying them was the original plan for making remote players' power-ups
    // appear, and it does not work: the engine's byte is a one-frame rising edge
    // written inside the input tick, so a 30 Hz sample misses most presses, and
    // writing it from a packet turns the edge into a level that can fire
    // repeatedly. PKT_USE replaces it with the actual outcome.
    //
    // Leaving them in would now be a SECOND source firing the same card - so they
    // go. LookBack, ShootBack, PowerSlide and the horn stay: they are modifiers
    // and cosmetics, not triggers.
    st.buttons = (uint8_t)(p->state.buttons & ~0x1Cu);
    ApplyInputs(v, st);
}

// ---- capture (Build phase) -------------------------------------------------
static void OnCapture(float dt, void*) {
    if (!g_raceLive || !net::Ready() || !HaveId()) return;
    // The race clock advances EVERY frame: it is the timeline the receiver
    // interpolates on, so it has to be continuous, not one tick per packet.
    g_raceTime += dt;
    g_sendAcc += dt;
    const float iv = 1.0f / float(g_cfg.sendHz > 0 ? g_cfg.sendHz : 30);
    if (g_sendAcc < iv) return;
    g_sendAcc -= iv;                 // subtract, not zero: keeps the rate honest

    VehicleEntity me = LocalPlayerEntity();
    VehicleState st{};
    if (!me || !CaptureState(me, st)) return;
    net::StateBody b{};
    b.raceId = g_raceId;
    b.tick   = ++g_tick;
    b.timeUs = (uint32_t)(g_raceTime * 1e6);   // microseconds: at 50 m/s a 1 ms
                                               // rounding is 5 cm of timeline error
    memcpy(b.pos, st.pos, sizeof(b.pos));   memcpy(b.quat, st.quat, sizeof(b.quat));
    memcpy(b.lin, st.lin, sizeof(b.lin));   memcpy(b.ang,  st.ang,  sizeof(b.ang));
    b.steer = st.steer; b.throttle = st.throttle; b.buttons = st.buttons; b.flags = st.flags;
    // Bits 1-2 of an already-existing field carry our race-progress bucket (0/1/2).
    // No new packet, no size change, no version bump: bit 0 is the teleport hint
    // and the rest of the word has never been used. Bubble types are drawn from
    // PowerUpWeights[stage], and the stage is computed per machine, so one
    // machine's has to win or two players see different bubbles.
    {
        const int rs = RaceStage();
        if (rs >= 0) b.flags = (b.flags & ~0x6u) | ((uint32_t)(rs & 3) << 1);
        // Bits 3-10: which reroll window we are in, so a bubble that respawns
        // does not come back as the same type it was last time. Coarse on
        // purpose - two machines respawn the same bubble a fraction of a second
        // apart, and a wide window means they almost always land in the same one.
        // When they do not, the next respawn puts them back in step.
        const uint32_t ep = (uint32_t)(g_raceTime / kBubbleEpochSec) & 0xffu;
        b.flags = (b.flags & ~0x7f8u) | (ep << 3);
    }

    // Where we were, for the victim half of the plausibility gate. Send rate is
    // the right rate for this: it is the resolution the shooter saw us at.
    RememberOurPosition(g_raceTime, st.pos);

    // What we are HOLDING. Cheap - two slots, read straight off the controller -
    // and it is the only thing that makes everyone's copy of our car fire the
    // same weapon we do.
    if (g_rules.syncPowerUps) {
        PowerUpSlot held[2];
        ReadPowerUpSlots(me, held);
        for (int i = 0; i < 2; ++i) {
            b.slots[i].nameHash = catalog::PowerUpHashOf(held[i].powerUp);
            b.slots[i].level    = (uint8_t)(held[i].level  < 0 ? 0 :
                                            held[i].level  > 12 ? 12 : held[i].level);
            b.slots[i].count    = (uint8_t)(held[i].count  < 0 ? 0 :
                                            held[i].count  > 255 ? 255 : held[i].count);
            // Holding something we cannot name is NOT the same as holding
            // nothing, and saying "empty" would make every other machine strip
            // their copy and show us firing blanks. Say "unknown" instead, which
            // means leave yours alone.
            if (!b.slots[i].nameHash && held[i].powerUp)
                b.slots[i].nameHash = net::kSlotUnknown;
            if (!b.slots[i].nameHash) { b.slots[i].level = 0; b.slots[i].count = 0; }
        }
    }

    // What is affecting US, straight from the engine's own controller. This is
    // the authoritative half of effect replication: nobody else gets a vote on
    // our car's status, so no two machines can disagree for longer than one
    // packet - and a phantom effect somebody else applied locally corrects
    // itself 33 ms later.
    if (g_rules.syncEffects) {
        // 32 is well past anything real - the engine warns past 64 active and a
        // car normally has one or two - so in practice this reads them all and
        // the sort below really does pick the longest-running.
        ActiveEffect act[32];
        int n = ReadVehicleEffects(me, act, 32);
        // More than fits on the wire: keep the ones with the longest to run,
        // since those are the ones still worth telling anyone about.
        if (n > net::kMaxWireEffects) {
            std::sort(act, act + n, [](const ActiveEffect& x, const ActiveEffect& y) {
                return x.timeLeft > y.timeLeft;
            });
            n = net::kMaxWireEffects;
        }
        b.effectCount = (uint8_t)n;
        for (int i = 0; i < n; ++i) {
            b.effects[i].nameHash = act[i].nameHash;
            const float t = act[i].timeLeft;
            // 255 means "longer than this field can say" - a permanent effect
            // has a duration of 1e9 and would otherwise wrap to nothing.
            b.effects[i].tenths = (t >= 25.4f || t < 0.f) ? 255
                                : (uint8_t)(t * 10.f);
        }
    }
    net::SendState(b);
}

// A fresh seed per race. It only has to differ between races and reach everyone,
// which it does - the seed rides in the RACE packet and every client adopts it -
// so the source just needs to not repeat. Never zero: VuRand(0) takes a clock
// branch and moves a global, which is the one value that would NOT be shared.
static uint64_t MakeSeed(uint32_t raceId) {
    LARGE_INTEGER pc{}; QueryPerformanceCounter(&pc);
    uint64_t x = (uint64_t)pc.QuadPart;
    x ^= (uint64_t)GetTickCount64() * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t)GetCurrentProcessId() << 32;
    x ^= (uint64_t)raceId * 0xBF58476D1CE4E5B9ull;
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x ? x : 1;
}

// Push our ini race spec to everyone and load it ourselves.
static net::RaceBody BuildRaceBody(uint32_t raceId) {
    net::RaceBody rb{};
    strncpy_s(rb.track, g_cfg.track, sizeof(rb.track)-1);
    strncpy_s(rb.setting, g_cfg.setting, sizeof(rb.setting)-1);
    rb.gameType = GameTypeIndex(g_cfg.gameType);
    if (rb.gameType < 0) { Log("[mp] unknown gametype \"%s\", using Race", g_cfg.gameType); rb.gameType = 0; }
    rb.lapCount = g_cfg.laps;
    rb.mirror = g_cfg.mirror ? 1 : 0;
    // The grid must be at least as big as the lobby, or the players who do not
    // fit have no car and no spawn point.
    // By the HIGHEST live id, not the head count. Ids are handed out lowest-free
    // and go sparse as people leave, so two players can be slots 0 and 15 - and a
    // 2-car grid would leave slot 15 with no car and no spawn point.
    int want = g_cfg.players;
    const int need = net::HighestLiveId() + 1;
    if (want < need) want = need;
    if (want < LobbySize()) want = LobbySize();
    rb.vehicleCount = want;
    if (g_cfg.randomSeed) {
        g_cfg.seed = MakeSeed(raceId);
        Log("[mp] race %u seed %llu (rolled fresh - set randomseed = false to pin it)",
            raceId, (unsigned long long)g_cfg.seed);
    }
    rb.seed = g_cfg.seed;
    rb.raceId = raceId;
    rb.hostEpoch = g_myEpoch;
    // Whoever calls the race sets the hit rules for it. When we are hosting from
    // the game, that is our own ini - which is why the keys stay in bbr2_mp.ini
    // and are not simply deleted in favour of the server's.
    rb.rules = RulesFromConfig();
    return rb;
}

// ---- start / go, whoever is in charge --------------------------------------
// On a host these do the thing. On an admin client attached to a DEDICATED
// SERVER they ask the server to do it, and the RaceBody we send is built from
// our own overlay settings - so whoever has the panel open drives the lobby,
// rather than whoever happens to own the socket.
static void RequestOrBeginRace(const char* who) {
    if (net::IsHost()) { BeginRace(who); return; }
    if (!(net::ServerFlags() & net::SF_HEADLESS) || !net::IsAdmin()) {
        Log("[mp] %s ignored - the host starts races", who);
        char msg[96];
        sprintf_s(msg, "%s starts the race", StarterName());
        Notify(msg);
        return;
    }
    // raceId and hostEpoch are the server's to assign; ours would be meaningless.
    net::RaceBody rb = BuildRaceBody(0);
    net::SendRequest(net::REQ_START, rb);
    Log("[mp] %s: asked the server to start track=\"%s\" laps=%d",
        who, rb.track[0] ? rb.track : "(the server's own default)", rb.lapCount);
    Notify("Asked the server to start a race");
}

static void RequestOrDropLights(const char* who) {
    // Only a DEDICATED SERVER takes requests. When the host is a game it has F9
    // and F10 in front of it, and F10 here has always been the client's local
    // unstick for a lost GO - routing that to a host which never drains a request
    // queue would leave the player on the load gate forever.
    const bool remote = !net::IsHost() &&
                        (net::ServerFlags() & net::SF_HEADLESS) && net::IsAdmin();
    if (!remote) {
        // A client does NOT get to start the race, not even for itself.
        //
        // This used to fall through to the local DropTheLights, on the reasoning
        // that F10 was the client's escape hatch for a GO that went missing. That
        // was the wrong call: the person running the race may be deliberately
        // waiting for somebody, and a client that unsticks itself drives off alone
        // while everyone else is still parked. Because a dedicated server ignores
        // GOs from clients, the only person who could see that desync was the one
        // causing it.
        //
        // A lost GO is already covered by retransmission - the server repeats it
        // 40 times over 10 seconds and the host resends the whole race body every
        // half second - so the escape hatch was insuring against something that
        // does not happen, at the cost of something that does.
        if (!WeStartRaces()) {
            Log("[mp] %s ignored - %s starts the race.", who, StarterName());
            char msg[128];
            sprintf_s(msg, "%s starts the race", StarterName());
            Notify(msg);
            return;
        }
        DropTheLights(who);
        return;
    }
    net::SendRequest(net::REQ_GO, net::RaceBody{});
    Log("[mp] %s: asked the server to go", who);
}

static void CallRace(bool holdAtLoad) {
    net::RaceBody rb = BuildRaceBody(++g_raceId);
    net::SendRace(rb);
    AdoptRules(rb.rules, false, "our race, our rules");

    // Seed BEFORE StartRace: StartRace calls prepareQuick, and prepareQuick is
    // what rolls the AI roster. Reseeding in the begin() hook is a whole config
    // rebuild too late - the opponents have already been chosen.
    SetBubbleSeed(g_cfg.seed); SeedRng(g_cfg.seed);

    RaceSpec spec{};
    strncpy_s(spec.track, rb.track, sizeof(spec.track)-1);
    strncpy_s(spec.setting, rb.setting, sizeof(spec.setting)-1);
    spec.gameType = rb.gameType; spec.lapCount = rb.lapCount; spec.mirror = g_cfg.mirror;
    spec.vehicleCount = rb.vehicleCount;
    g_askedFor = spec;
    if (StartRace(spec, holdAtLoad)) {
        g_phase = LOADING; g_phaseAcc = 0.f;
    } else {
        Log("[mp] StartRace failed - releasing players");
        // The countdown hold is armed BEFORE StartRace. Leaving it latched means
        // the next race - including one started from the game's own menus - has
        // its 3-2-1 neutralised with nothing that will ever release it.
        g_holdCountdown = false; ReleaseCountdown();
        ReleaseStartGate(); ReleaseLoadGate();
        SendGoReliably(1);
        g_weOwnRace = false;
    }
}

// ---- per-player nameplates ------------------------------------------------
// The engine re-reads the label every frame with no caching, so this updates
// live. Only write when the text actually changes - each write is a std::string
// assign through the engine.
//
// The engine's label strings understand an inline color escape: "{[r,g,b]}"
// recolors everything after it, and it lasts to the end of the string. So the
// name stays in the game's own color and only the status tag goes grey.
static const char kGrey[] = "{[125,125,125]}";

static void UpdateLabels() {
    if (!g_raceLive) return;
    const int me = HaveId() ? MyId() : 0;
    for (int i = 0; i < net::kMaxPeers; ++i) {
        if (i == me) continue;
        net::Peer* p = net::GetPeer(i);
        RemotePlayer& rp = g_rp[i];
        if (!rp.inThisRace || !rp.cfg || !p || !net::PeerAlive(i) || !p->helloSeen) continue;
        char want[96];
        const bool loading = (p->flags & net::PF_LOADING) && !(p->flags & net::PF_READY);
        if (loading)                sprintf_s(want, "%s %s[LOADING...]", p->name, kGrey);
        else if (!p->haveState && !(p->flags & net::PF_RACING))
                                    sprintf_s(want, "%s %s[WAITING]", p->name, kGrey);
        else                        sprintf_s(want, "%s", p->name);
        if (strcmp(rp.shownName, want) != 0) {
            strncpy_s(rp.shownName, want, sizeof(rp.shownName)-1);
            SetVehicleName(rp.cfg, want);
        }
    }
}

// ---- overlay --------------------------------------------------------------
// PublishGui runs on the game thread. OverlayPanel runs on the RENDER thread,
// inside the game's Present call. The panel may read the snapshot and nothing
// else - no engine calls, no net:: calls, no touching g_rp. That separation is
// the whole reason the overlay is safe to add at this stage.
static uint64_t g_guiStartMs = 0;
static uint32_t g_guiTicks   = 0;

// strcat_s does NOT truncate - it zeroes the destination and calls the
// invalid-parameter handler, and the default handler kills the process. Four
// concurrent effect names overflow a 64-byte buffer, so joining them with
// strcat_s would have taken the game down the first time somebody grabbed a
// shield while frozen and oiled.
static void AppendTrunc(char* dst, size_t cap, const char* sep, const char* src) {
    if (!dst || !cap || !src || !src[0]) return;
    size_t len = strnlen(dst, cap);
    if (dst[0] && sep) {
        for (const char* p = sep; *p && len + 1 < cap; ++p) dst[len++] = *p;
    }
    for (const char* p = src; *p && len + 1 < cap; ++p) dst[len++] = *p;
    dst[len] = 0;
}

static void PublishGui(float dt) {
    if (!gui::Installed()) return;

    // The whole snapshot is built here, on the game thread, and copied whole.
    // Nothing the panel draws is a pointer into something the game owns - that
    // is the rule the overlay's safety rests on, and this function is where it
    // is kept.
    static gui::Snapshot s{};
    s.gameTicks = ++g_guiTicks;
    s.dt        = dt;
    s.uptime    = (double)(GetTickCount64() - g_guiStartMs) / 1000.0;
    s.isHost    = net::IsHost();
    s.headlessHost = !net::IsHost() && (net::ServerFlags() & net::SF_HEADLESS) != 0;
    s.isAdmin   = net::IsAdmin();
    s.myId      = HaveId() ? MyId() : -1;
    s.maxPlayers= net::MaxPlayers();
    s.lobbySize = LobbySize();
    s.readyCount= LobbyReady();
    s.raceLive  = g_raceLive;
    s.phase     = (int)g_phase;
    s.raceId    = g_raceId;
    s.port      = g_cfg.port;
    s.protocol  = net::kProtocolVersion;
    s.countdownHeld = CountdownHeld();
    s.preGameHeld   = PreGameHeld();
    s.startGateHeld = StartGateHeld();
    s.loadGateHeld  = LoadGateHeld();
    s.holdMode      = g_cfg.holdMode;
    s.spectating    = g_spectate;

    // What F9 would start. This comes from the ini, NOT from the engine: at the
    // main menu VuGameConfig still holds whatever the front end last put there,
    // which is why the live numbers read as defaults until a race is running.
    strncpy_s(s.wantTrack,   g_cfg.track,    sizeof(s.wantTrack)-1);
    strncpy_s(s.wantSetting, g_cfg.setting,  sizeof(s.wantSetting)-1);
    strncpy_s(s.wantGameType,g_cfg.gameType, sizeof(s.wantGameType)-1);
    s.wantLaps   = g_cfg.laps;
    s.wantGrid   = g_cfg.players;
    s.wantMirror = g_cfg.mirror;

    // Legality of each command, decided here and only advisory in the panel -
    // the game thread re-checks all of it before acting.
    // Legality of each command, decided here and only advisory in the panel - the
    // game thread re-checks all of it before acting. Two shapes now: we are the
    // host and act locally, or we are an admin on a dedicated server and ask it
    // to act. Everyone else gets a reason instead of a dead button.
    const bool remoteAdmin = !net::IsHost() && net::IsAdmin();

    s.canStart = true; s.cantStartWhy[0] = 0;
    if (!net::IsAdmin()) {
        s.canStart = false;
        strcpy_s(s.cantStartWhy, (net::ServerFlags() & net::SF_HEADLESS)
                 ? "the server does not list you as an admin"
                 : "only the host starts races - you will be pulled in");
    } else if (g_raceLive) {
        s.canStart = false;
        strcpy_s(s.cantStartWhy, "a race is already running");
    } else if ((g_phase == LOADING || g_phase == HELD) && g_phaseAcc < 60.f) {
        s.canStart = false;
        strcpy_s(s.cantStartWhy, "a race is already being set up");
    } else if (!remoteAdmin && !g_cfg.track[0]) {
        // On a server the track can come from the server's own ini, so a blank
        // one here is not a reason to refuse.
        s.canStart = false;
        strcpy_s(s.cantStartWhy, "[race] track is blank in bbr2_mp.ini");
    }

    s.canGo = true; s.cantGoWhy[0] = 0;
    if (!net::IsAdmin()) {
        // F10 still works as a manual unstick, but it only releases YOUR gates,
        // which desyncs you from everyone else - not something to offer as a button.
        s.canGo = false;
        strcpy_s(s.cantGoWhy, "the host releases the gate");
    } else if (g_raceId == 0) {
        s.canGo = false;
        strcpy_s(s.cantGoWhy, remoteAdmin ? "no race has been announced yet"
                                          : "no race has been called yet");
    } else if (g_raceLive && !SomethingIsHolding()) {
        s.canGo = false;
        strcpy_s(s.cantGoWhy, "the race is already running - nothing is holding it");
    } else if (!remoteAdmin && !g_raceLive && !LoadGateHeld() &&
               g_phase != LOADING && g_phase != HELD) {
        s.canGo = false;
        strcpy_s(s.cantGoWhy, "nothing is waiting to be released");
    }

    // Two things here are worth more than one tick of work, so they refresh on
    // their own slower cadence: reading the live race spec copies strings out of
    // the engine, and the gap-to-each-player needs our own state captured.
    if ((g_guiTicks % 30) == 1) {
        RaceSpec cur{}; GetCurrentRace(cur);
        strncpy_s(s.track,   cur.track,   sizeof(s.track)-1);
        strncpy_s(s.setting, cur.setting, sizeof(s.setting)-1);
        s.gameType = cur.gameType;
        s.lapCount = cur.lapCount;
        const char* gt = GameTypeName(cur.gameType);
        strncpy_s(s.gameTypeName, gt ? gt : "?", sizeof(s.gameTypeName)-1);
        strncpy_s(s.askedTrack, g_askedFor.track, sizeof(s.askedTrack)-1);
        s.askedGameType = g_askedFor.gameType;
        s.askedLaps     = g_askedFor.lapCount;
    }
    static float gap[net::kMaxPeers];
    static bool  gapInit = false;
    if (!gapInit) { for (float& g : gap) g = -1.f; gapInit = true; }
    if (g_raceLive && (g_guiTicks % 10) == 0) {
        VehicleState mine{};
        const bool haveMine = CaptureState(LocalPlayerEntity(), mine);
        for (int i = 0; i < net::kMaxPeers; ++i) {
            float tp[3]{};
            if (!haveMine || !g_rp[i].ent || !net::PeerAlive(i) ||
                !RemoteSyncTarget(g_rp[i].sync, tp)) {
                gap[i] = -1.f; continue;
            }
            const float dx = tp[0]-mine.pos[0], dy = tp[1]-mine.pos[1], dz = tp[2]-mine.pos[2];
            gap[i] = sqrtf(dx*dx + dy*dy + dz*dz);
        }
    } else if (!g_raceLive) {
        for (float& g : gap) g = -1.f;
    }

    net::NetStats ns{}; net::GetStats(ns);
    s.rxTotal = ns.rxTotal; s.rxGood = ns.rxGood; s.rxJunk = ns.rxJunk;
    s.rxBadVersion = ns.rxBadVersion; s.rxSelf = ns.rxSelf;
    s.txTotal = ns.txTotal; s.relayed = ns.relayed;
    s.hitsSent = ns.hitsSent; s.hitsRecv = ns.hitsRecv; s.hitsRelayed = ns.hitsRelayed;
    s.hitsRefused = ns.hitsRefused; s.hitsDupes = ns.hitsDupes;
    s.hitsUndeliverable = ns.hitsUndeliverable; s.hitsDropped = ns.hitsDropped;
    s.rulesFromHost = g_rulesFromHost;
    s.rulesEffects  = g_rules.syncEffects != 0;
    s.rulesClaims   = g_rules.hitClaims != 0;
    s.rulesGrace    = g_rules.effectGrace;   s.rulesTimeout  = g_rules.hitTimeout;
    s.rulesCooldown = g_rules.hitCooldown;
    s.rulesMaxAge   = g_rules.hitMaxAge;     s.rulesMaxDrift = g_rules.hitMaxDrift;
    strncpy_s(s.hostAddr, ns.hostAddr, sizeof(s.hostAddr)-1);

    s.rows = 0;
    for (int i = 0; i < net::kMaxPeers && s.rows < 16; ++i) {
        net::Peer* p = net::GetPeer(i);
        if (!p || !net::PeerAlive(i)) continue;
        gui::PlayerRow& r = s.p[s.rows++];
        r.id        = i;
        strncpy_s(r.name,    p->name,    sizeof(r.name)-1);
        strncpy_s(r.vehicle, p->vehicle, sizeof(r.vehicle)-1);
        strncpy_s(r.driver,  p->driver,  sizeof(r.driver)-1);
        strncpy_s(r.paint,   p->paint,   sizeof(r.paint)-1);
        strncpy_s(r.paintDetail, p->paintDetail, sizeof(r.paintDetail)-1);
        strncpy_s(r.decal,   p->decal,   sizeof(r.decal)-1);
        r.flags      = p->flags;
        r.isSelf     = (i == MyId());
        r.isHost     = (i == net::kHostId);
        r.helloSeen  = p->helloSeen;
        r.inThisRace = g_rp[i].inThisRace;
        r.ready      = p->ready && p->readyRace == g_raceId;
        r.loading    = (p->flags & net::PF_LOADING) != 0;
        r.racing     = (p->flags & net::PF_RACING) != 0;
        r.haveState  = p->haveState;
        r.ageS       = net::PeerAge(i);
        r.packetMs   = g_rp[i].sync.interval * 1000.f;
        r.bufferMs   = g_rp[i].sync.lead     * 1000.f;
        r.jitterMs   = g_rp[i].sync.jitter   * 1000.f;
        r.gapM       = gap[i];
        r.lastTick   = p->lastTick;
        r.entIndex   = -1;
        r.canSpectate = false;
        // Their own report of what is affecting them, resolved back to names.
        r.effects[0] = 0;
        {
            const int ne = (p->state.effectCount <= net::kMaxWireEffects)
                         ? p->state.effectCount : net::kMaxWireEffects;
            for (int e = 0; e < ne; ++e)
                AppendTrunc(r.effects, sizeof(r.effects), ", ",
                            catalog::EffectName(p->state.effects[e].nameHash));
        }
        if (r.isSelf) {
            // Our own row has no network numbers by definition; showing zeros
            // would read as a dead link.
            r.packetMs = r.bufferMs = r.jitterMs = -1.f;
            r.gapM = -1.f;
            // ...and our own status has to come from LOCAL state. The flag bits
            // are the host's view of everyone else: PKT_LOBBY skips our own id,
            // and only the host ever calls SetAllFlags. Reading them for
            // ourselves left a client's own row saying "next race" for the whole
            // race it was driving in.
            r.helloSeen  = true;
            r.inThisRace = g_raceLive;
            r.effects[0] = 0;
            if (g_raceLive) {
                ActiveEffect act[net::kMaxWireEffects];
                const int ne = ReadVehicleEffects(LocalPlayerEntity(), act, net::kMaxWireEffects);
                for (int e = 0; e < ne; ++e)
                    AppendTrunc(r.effects, sizeof(r.effects), ", ",
                                catalog::EffectName(act[e].nameHash));
            }
            r.racing     = (g_phase == RACING);
            r.loading    = (g_phase == LOADING);
            r.ready      = (g_phase == HELD) ||
                           (p->ready && p->readyRace == g_raceId);
        }
    }
    // Every car on the grid, in entity order. Cheap - EntityAt is a pointer read
    // out of VuBaseGame's own vector - and it is what lets the panel offer an AI
    // car to watch when nobody else is connected.
    s.carCount = 0;
    if (g_raceLive) {
        VehicleEntity mine = LocalPlayerEntity();
        const int n = EntityCount();
        for (int i = 0; i < n && s.carCount < 24; ++i) {
            VehicleEntity e = EntityAt(i);
            if (!e) continue;
            Vehicle cv = e.config();
            gui::Snapshot::Car& car = s.cars[s.carCount++];
            const char* nm = cv ? GetVehicleName(cv) : nullptr;
            StripColor(car.name, sizeof(car.name), nm && nm[0] ? nm : "(unnamed)");
            const char* vh = cv ? GetVehicleString(cv, 0) : nullptr;
            strncpy_s(car.vehicle, vh ? vh : "", sizeof(car.vehicle)-1);
            car.isAi = cv ? cv.isAi() : true;
            car.isMe = (e.p == mine.p);
            car.peerId = -1;
            for (int j = 0; j < net::kMaxPeers; ++j)
                if (g_rp[j].ent && g_rp[j].ent.p == e.p) { car.peerId = j; break; }
            // Point each roster row at its car, so "Watch this player" knows
            // which entity to name.
            const int who = car.isMe ? MyId() : car.peerId;
            for (int k = 0; k < s.rows; ++k)
                if (s.p[k].id == who) { s.p[k].entIndex = i; s.p[k].canSpectate = true; }
        }
    }

    gui::Publish(s);

    // Clicks that never reached us. Only possible if the game thread stopped
    // ticking long enough to fill a 32-deep queue, which is worth knowing about.
    {
        static uint32_t lastDropped = 0;
        const uint32_t d = gui::CommandsDropped();
        if (d != lastDropped) {
            Log("[gui] %u overlay command(s) dropped - the game thread was not "
                "ticking", d - lastDropped);
            lastDropped = d;
        }
    }

    // If Present never fires, the vtable we patched is not the one the game
    // actually uses. Say so once, loudly, instead of leaving a dead key.
    // Presenting(), not Drawing(). Drawing() only goes true once the panel has
    // actually been rendered, i.e. once somebody presses F6 - so this used to tell
    // every player who had not opened the overlay yet that the swapchain hook had
    // failed, and advise them to turn the overlay off. It was wrong on exactly the
    // machines where it was working.
    static bool moaned = false;
    if (!moaned && !gui::Presenting() && s.uptime > 20.0) {
        moaned = true;
        Log("[gui] hooks are installed but no frame has come through after 20s.");
        Log("[gui]   The game's swapchain is probably not using the vtable we");
        Log("[gui]   patched. Set overlay = 0 in bbr2_mp.ini if it is in the way.");
    }
}

// ---- writing the ini back -------------------------------------------------
// In place, key by key. The file is full of comments the user may have added to
// and the section layout is meaningful, so this rewrites only the value of keys
// it recognises and copies every other byte through unchanged. A key that is not
// in the file is appended at the end with a marker, rather than guessed into a
// section it might not belong to.
static bool SaveIni() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* sl = strrchr(path, '\\')) { sl[1] = 0; strcat_s(path, "bbr2_mp.ini"); }

    struct KV { const char* key; char val[80]; };
    KV want[] = {
        {"track",    ""}, {"setting", ""}, {"gametype", ""}, {"laps", ""},
        {"players",  ""}, {"mirror",  ""}, {"holdstart", ""},
        {"vehicle",  ""}, {"driver",  ""}, {"paint", ""}, {"paintdetail", ""},
        {"decal",    ""}, {"name",    ""},
    };
    auto set = [&](int i, const char* v) { strncpy_s(want[i].val, v, sizeof(want[i].val)-1); };
    char num[32];
    set(0, g_cfg.track);   set(1, g_cfg.setting);  set(2, g_cfg.gameType);
    sprintf_s(num, "%d", g_cfg.laps);    set(3, num);
    sprintf_s(num, "%d", g_cfg.players); set(4, num);
    set(5, g_cfg.mirror ? "true" : "false");
    // holdstart is a word in the file but a 3-bit mask in memory, and only five
    // of the eight masks have a word. Writing the nearest word for the other
    // three would silently change the setting on the next launch, so those are
    // written as the number - which LoadConfig now accepts.
    char holdBuf[16];
    const char* hold = nullptr;
    switch (g_cfg.holdMode) {
        case 0: hold = "off";     break;
        case 1: hold = "load";    break;
        case 2: hold = "grid";    break;
        case 3: hold = "both";    break;
        case 4: hold = "pregame"; break;
        default: sprintf_s(holdBuf, "%d", g_cfg.holdMode & 7); hold = holdBuf; break;
    }
    set(6, hold);
    set(7, g_cfg.vehicle);     set(8, g_cfg.driver); set(9, g_cfg.paint);
    set(10, g_cfg.paintDetail); set(11, g_cfg.decal); set(12, g_cfg.name);
    const int nWant = (int)(sizeof(want)/sizeof(want[0]));

    // Read the whole file first. A file we could not read is NOT the same as an
    // empty one: treating a locked or half-read ini as empty would replace the
    // user's whole configuration - role, port, seed, the [gui] section, every
    // comment - with the twelve keys below.
    std::string body;
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) {
            Log("[gui] bbr2_mp.ini exists but could not be opened - NOT saving, "
                "rather than replacing it with a stub");
            return false;
        }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
        const bool bad = ferror(f) != 0;
        fclose(f);
        if (bad) {
            Log("[gui] error reading bbr2_mp.ini - NOT saving");
            return false;
        }
    }

    std::string out;
    bool seen[sizeof(want)/sizeof(want[0])] = {};
    size_t i = 0;
    while (i <= body.size()) {
        size_t e = body.find('\n', i);
        const bool last = (e == std::string::npos);
        if (last) e = body.size();
        std::string line = body.substr(i, e - i);
        std::string keep = line;

        // Strip the trailing CR for parsing, put it back when we rewrite.
        std::string eol = "";
        if (!line.empty() && line.back() == '\r') { eol = "\r"; line.pop_back(); }

        size_t a = line.find_first_not_of(" \t");
        if (a != std::string::npos && line[a] != ';' && line[a] != '#' && line[a] != '[') {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(a, eq - a);
                while (!k.empty() && (k.back()==' '||k.back()=='\t')) k.pop_back();
                for (int j = 0; j < nWant; ++j) {
                    if (_stricmp(k.c_str(), want[j].key) != 0) continue;
                    // EVERY occurrence, not just the first: LoadConfig lets the
                    // last one win, so rewriting only the first would leave a
                    // duplicate further down quietly overriding the save.
                    seen[j] = true;
                    keep = line.substr(0, a) + want[j].key + " = " + want[j].val + eol;
                    break;
                }
            }
        }
        out += keep;
        if (!last) out += '\n';
        if (last) break;
        i = e + 1;
    }

    std::string appended;
    for (int j = 0; j < nWant; ++j)
        if (!seen[j]) { appended += want[j].key; appended += " = "; appended += want[j].val; appended += "\r\n"; }
    if (!appended.empty())
        out += "\r\n; --- added by the overlay ---\r\n" + appended;

    // Write to a temporary file and swap, so a failure halfway cannot leave the
    // user with half an ini and no way back.
    char tmp[MAX_PATH]; sprintf_s(tmp, "%s.tmp", path);
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") != 0 || !f) { Log("[gui] could not write %s", tmp); return false; }
    const bool wrote = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!wrote) { DeleteFileA(tmp); Log("[gui] short write saving the ini"); return false; }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        Log("[gui] could not replace bbr2_mp.ini (is it open in an editor?)");
        return false;
    }
    Log("[gui] saved bbr2_mp.ini");
    return true;
}

// ---- spectating -----------------------------------------------------------
// The camera is bound per viewport to a VuVehicleEntity, and retargeting it is a
// single engine call that has nothing to do with pausing - the game's own
// post-game camera does exactly this. So watching another player live costs one
// call, not a camera implementation.
//
// It has to be re-asserted, though: VuBaseGame::begin/end and the post-game
// camera entity both write the target themselves. Rather than call it blindly
// every tick and fight the engine's own unlink/relink, compare first.
static VehicleEntity SpectateTargetEntity() {
    if (g_spectate < 0) return LocalPlayerEntity();
    // An index into the live entity list, not a peer id: every car on the grid
    // is addressable, AI included. That is what makes this testable with nobody
    // else connected, and it is the more useful feature anyway.
    return EntityAt(g_spectate);
}

static void TickSpectate() {
    if (!g_raceLive) { g_spectate = -1; return; }
    VehicleEntity want = SpectateTargetEntity();
    if (!want) {
        if (g_spectate >= 0) {
            // They left, or their car went away with the race.
            Log("[gui] spectate target is gone - back to your own car");
            g_spectate = -1;
        }
        return;
    }
    if (EntityForViewport(0).p == want.p) return;
    SetCameraTarget(0, want);
}

// Our identity changed: tell everyone straight away rather than waiting for the
// two-second HELLO heartbeat, so the lobby updates while you are still looking
// at it.
// -1 means "the game's default", which is a blank string. Anything else has to
// be a live index into the list it names.
static bool SetIdentityField(char* dst, size_t cap, int idx, int count,
                             const char* (*name)(int), const char* what) {
    if (idx < -1 || idx >= count) {
        Log("[gui] %s index %d out of range - ignored", what, idx);
        return false;
    }
    if (idx < 0) dst[0] = 0;
    else         strncpy_s(dst, cap, name(idx), cap - 1);
    return true;
}

// Set when the identity changes, cleared when it has been written. The write is
// deferred rather than immediate: picking a car usually means clicking through a
// few of them, and rewriting the ini on every click would be a file rewrite per
// combo entry for no benefit.
static float g_saveIdentityIn = -1.f;

static void IdentityChanged(const char* what) {
    net::SetLocalIdentity(MyIdentity());
    Hello();
    Log("[gui] %s changed - broadcast; takes effect on the next race", what);
    if (g_cfg.autoSave) g_saveIdentityIn = 2.0f;
}

// The race settings are the same deal: the ini is a defaults file, and a track
// you picked in the panel should still be picked next launch.
static void SettingsChanged() { if (g_cfg.autoSave) g_saveIdentityIn = 2.0f; }

// Game thread. Writes the ini a couple of seconds after you stop changing things.
static void TickAutoSave(float dt) {
    if (g_saveIdentityIn < 0.f) return;
    g_saveIdentityIn -= dt;
    if (g_saveIdentityIn > 0.f) return;
    g_saveIdentityIn = -1.f;
    if (SaveIni()) Log("[gui] identity saved to bbr2_mp.ini");
    // SaveIni logs its own reason on failure, and refuses rather than writing a
    // stub, so a locked file costs you the save and nothing else.
}

// Runs on the game thread, at the top of the tick. Every guard the hotkey path
// applies is applied again here: the panel greys its buttons out, but the panel
// is a different thread and a frame behind, so it is advice, not authority.
static void DrainGuiCommands() {
    if (!gui::Installed()) return;
    static bool wrongThread = false;
    if (!wrongThread && gui::PopCommandThreadIsWrong()) {
        wrongThread = true;
        Log("[gui] WARNING: commands are being drained from a thread other than the "
            "one that publishes snapshots - that should be impossible");
    }
    // Bounded. A game-thread stall while the render thread keeps drawing lets a
    // user queue clicks faster than they can be run; executing all of them in the
    // first tick afterwards is its own hitch.
    gui::Command c;
    for (int budget = 8; budget > 0 && gui::PopCommand(c); --budget) {
        switch (c.type) {
        case gui::CMD_START_RACE:
            if (!net::IsAdmin()) {
                Log("[gui] start ignored - not the host, and not an admin here");
                char msg[96];
                sprintf_s(msg, "%s starts the race", StarterName());
                Notify(msg);
            } else if (g_raceLive) {
                Log("[gui] start ignored - a race is already running");
            } else if (net::IsHost() && !g_cfg.track[0]) {
                Log("[gui] start ignored - [race] track is blank in bbr2_mp.ini");
                Notify("No track set in bbr2_mp.ini");
            } else {
                RequestOrBeginRace("overlay");
            }
            break;
        case gui::CMD_GO:
            // BeginRace and DropTheLights carry their own guards, which is where
            // they belong - the button being lit is advice from a thread that is
            // a frame behind, never permission.
            if (!net::IsAdmin()) Log("[gui] GO ignored - only the host releases the gate");
            else                 RequestOrDropLights("overlay");
            break;
        case gui::CMD_SET_HOLD:
            g_cfg.holdMode = c.a & 7;
            Log("[gui] holdstart mode is now %d (takes effect on the NEXT race)",
                g_cfg.holdMode);
            break;
        case gui::CMD_TOAST_STATUS: StatusToast(); break;
        case gui::CMD_DUMP_LOG:     LogStatus();   break;

        // ---- race settings (host) ----
        // A client changing these would only desync itself from the race it is
        // about to be told to load, so they are refused rather than ignored.
        // The accessors return "" for a bad index, never null, so `if (const char*
        // x = ...)` would be a guard that never fires and would blank the setting
        // instead of refusing it. An index CAN be stale: it was captured on the
        // render thread, and an earlier command in the same burst may have been
        // dropped, so range-check against the list this command actually names.
        case gui::CMD_SET_TRACK:
            if (!net::IsAdmin()) { Log("[gui] track ignored - the host picks the race"); break; }
            if (c.a < 0 || c.a >= catalog::TrackCount()) {
                Log("[gui] track index %d out of range - ignored", c.a); break;
            }
            strncpy_s(g_cfg.track, catalog::Track(c.a), sizeof(g_cfg.track)-1);
            g_cfg.setting[0] = 0;      // settings belong to a track
            Log("[gui] track = %s", g_cfg.track);
            SettingsChanged();
            break;
        case gui::CMD_SET_SETTING: {
            if (!net::IsAdmin()) break;
            if (c.a < 0) { g_cfg.setting[0] = 0; Log("[gui] setting cleared"); break; }
            const int t = catalog::IndexOfTrack(g_cfg.track);
            if (t < 0 || c.a >= catalog::SettingCount(t)) {
                Log("[gui] setting index %d is not valid for \"%s\" - ignored",
                    c.a, g_cfg.track);
                break;
            }
            strncpy_s(g_cfg.setting, catalog::Setting(t, c.a), sizeof(g_cfg.setting)-1);
            Log("[gui] setting = %s", g_cfg.setting);
            SettingsChanged();
            break;
        }
        case gui::CMD_SET_GAMETYPE:
            if (!net::IsAdmin()) break;
            if (c.a < 0 || c.a >= catalog::GameTypeCount()) {
                Log("[gui] gametype index %d out of range - ignored", c.a); break;
            }
            strncpy_s(g_cfg.gameType, catalog::GameType(c.a), sizeof(g_cfg.gameType)-1);
            Log("[gui] gametype = %s", g_cfg.gameType);
            SettingsChanged();
            break;
        case gui::CMD_SET_LAPS:
            if (!net::IsAdmin()) break;
            g_cfg.laps = (c.a < 1) ? 1 : (c.a > 99 ? 99 : c.a);
            SettingsChanged();
            break;
        case gui::CMD_SET_GRID:
            if (!net::IsAdmin()) break;
            g_cfg.players = (c.a < 0) ? 0 : (c.a > 32 ? 32 : c.a);
            SettingsChanged();
            break;
        case gui::CMD_SET_MIRROR:
            if (!net::IsAdmin()) break;
            g_cfg.mirror = c.a != 0;
            SettingsChanged();
            break;

        // ---- identity (anyone) ----
        case gui::CMD_SET_VEHICLE:
            if (!SetIdentityField(g_cfg.vehicle, sizeof(g_cfg.vehicle), c.a,
                                  catalog::VehicleCount(), catalog::Vehicle, "vehicle")) break;
            IdentityChanged("vehicle");
            break;
        case gui::CMD_SET_DRIVER:
            if (!SetIdentityField(g_cfg.driver, sizeof(g_cfg.driver), c.a,
                                  catalog::DriverCount(), catalog::Driver, "driver")) break;
            IdentityChanged("driver");
            break;
        case gui::CMD_SET_PAINT:
            if (!SetIdentityField(g_cfg.paint, sizeof(g_cfg.paint), c.a,
                                  catalog::PaintCount(), catalog::Paint, "paint")) break;
            IdentityChanged("paint");
            break;
        case gui::CMD_SET_DECALCOLOR:
            if (!SetIdentityField(g_cfg.paintDetail, sizeof(g_cfg.paintDetail), c.a,
                                  catalog::DecalColorCount(), catalog::DecalColor,
                                  "decal color")) break;
            IdentityChanged("decal color");
            break;
        case gui::CMD_SET_DECAL:
            if (!SetIdentityField(g_cfg.decal, sizeof(g_cfg.decal), c.a,
                                  catalog::DecalCount(), catalog::Decal, "decal")) break;
            IdentityChanged("decal");
            break;

        case gui::CMD_SPECTATE: {
            if (!g_raceLive) { Log("[gui] spectate ignored - no race running"); break; }
            const int idx = c.a;
            VehicleEntity mine = LocalPlayerEntity();
            VehicleEntity want = (idx < 0) ? mine : EntityAt(idx);
            if (idx >= 0 && !want) {
                Log("[gui] spectate: no car at entity index %d - ignored", idx);
                break;
            }
            // Watching our own car is just "back to my car".
            g_spectate = (idx >= 0 && want.p != mine.p) ? idx : -1;
            want = SpectateTargetEntity();
            if (want) SetCameraTarget(0, want);
            if (g_spectate < 0) {
                Log("[gui] camera back on your own car");
                Notify("Back on your own car");
            } else {
                const char* nm = GetVehicleName(want.config());
                Log("[gui] spectating car %d (\"%s\")", g_spectate, nm ? nm : "?");
                char body[96];
                sprintf_s(body, "Watching %s - you are still driving", nm ? nm : "that car");
                Notify(body);
            }
            break;
        }
        case gui::CMD_SAVE_INI:
            if (SaveIni()) Notify("Settings saved to bbr2_mp.ini");
            else           Notify("Could not save bbr2_mp.ini - see the log");
            break;
        default: break;
        }
    }
}

static const char* PhaseName(int p) {
    switch (p) {
        case IDLE:    return "idle";
        case LOADING: return "loading";
        case HELD:    return "held at the gate";
        case RACING:  return "racing";
        default:      return "?";
    }
}

// ---- the panel (RENDER THREAD) --------------------------------------------
// Reads gui::Snapshot and nothing else. No engine calls, no net:: calls, no
// touching g_rp - see the rule at the top of bbr2_gui.h.
static const ImVec4 kDim  (0.62f, 0.62f, 0.62f, 1.f);
static const ImVec4 kGood (0.45f, 0.85f, 0.45f, 1.f);
static const ImVec4 kWarn (0.95f, 0.75f, 0.35f, 1.f);
static const ImVec4 kBad  (0.95f, 0.45f, 0.45f, 1.f);

static void StatusCell(const gui::PlayerRow& r, bool raceLive) {
    if (!r.helloSeen)            { ImGui::TextColored(kWarn, "connecting"); return; }
    if (r.racing)                { ImGui::TextColored(kGood, "racing");   return; }
    if (r.ready)                 { ImGui::TextColored(kGood, "ready");    return; }
    if (r.loading)               { ImGui::TextColored(kWarn, "loading");  return; }
    if (raceLive && !r.inThisRace) { ImGui::TextColored(kDim, "next race"); return; }
    ImGui::TextColored(kDim, "waiting");
}

static void AgeCell(float ageS) {
    if (ageS < 0.f)      ImGui::TextColored(kDim,  "-");
    else if (ageS < 1.f) ImGui::TextColored(kGood, "now");
    else if (ageS < 3.f) ImGui::TextColored(kWarn, "%.0fs", ageS);
    else                 ImGui::TextColored(kBad,  "%.0fs", ageS);
}

// A button that explains itself when it is disabled, instead of just being grey.
static bool GatedButton(const char* label, bool enabled, const char* why,
                        const ImVec2& size = ImVec2(0, 0)) {
    if (!enabled) ImGui::BeginDisabled();
    const bool hit = ImGui::Button(label, size);
    if (!enabled) ImGui::EndDisabled();
    if (!enabled && why && why[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why);
    return hit;
}

// Posting a command is the ONLY thing the panel does that reaches the game. A
// full queue is reported by the game thread from gui::CommandsDropped(), so
// there is nothing useful to do with the return value here.
static bool Post(gui::CmdType t, int a = 0) { return gui::PushCommand(gui::Command{t, a}); }

// Which row the user clicked, by player id. -1 = none. Render-thread only.
static int g_selRow = -1;

static void PlayerTable(const gui::Snapshot& s) {
    const ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("players", 7, f)) return;
    ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 22.f);
    ImGui::TableSetupColumn("player",  ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("car",     ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("status",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("heard",   ImGuiTableColumnFlags_WidthFixed, 40.f);
    ImGui::TableSetupColumn("packet",  ImGuiTableColumnFlags_WidthFixed, 52.f);
    ImGui::TableSetupColumn("gap",     ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < s.rows; ++i) {
        const gui::PlayerRow& r = s.p[i];
        ImGui::TableNextRow();
        ImGui::PushID(r.id);

        ImGui::TableNextColumn();
        // A selectable spanning the row, so clicking anywhere opens the detail
        // block below. AllowOverlap keeps the other cells' tooltips working.
        char tag[8]; sprintf_s(tag, "%d", r.id);
        if (ImGui::Selectable(tag, g_selRow == r.id,
                              ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap))
            g_selRow = (g_selRow == r.id) ? -1 : r.id;

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.name[0] ? r.name : "(unnamed)");
        if (r.isSelf || r.isHost) {
            ImGui::SameLine();
            ImGui::TextColored(kDim, r.isSelf && r.isHost ? "(you, host)"
                                   : r.isSelf             ? "(you)" : "(host)");
        }

        ImGui::TableNextColumn();
        if (r.vehicle[0]) ImGui::TextUnformatted(r.vehicle);
        else              ImGui::TextColored(kDim, "default");

        ImGui::TableNextColumn();
        if (r.effects[0]) {
            ImGui::TextColored(kWarn, "%s", r.effects);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("active power-up effects");
        } else StatusCell(r, s.raceLive);
        ImGui::TableNextColumn(); AgeCell(r.ageS);

        ImGui::TableNextColumn();
        if (r.packetMs > 0.f) ImGui::Text("%.0f ms", r.packetMs);
        else                  ImGui::TextColored(kDim, "-");

        ImGui::TableNextColumn();
        if (r.gapM >= 0.f) ImGui::Text("%.0f m", r.gapM);
        else               ImGui::TextColored(kDim, "-");

        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---- dropdowns ------------------------------------------------------------
// A combo whose value actually lives on the game thread. The snapshot is a tick
// behind - and during a level load the game thread does not tick at all while
// this panel keeps drawing - so after posting a change we hold the new value
// locally until the snapshot agrees. Without this the box visibly snaps back to
// the old value for a moment, and a second change would be built on the stale one.
struct Pending { int want = -2; double since = 0.0; };

static int Resolve(Pending& p, int fromSnapshot) {
    if (p.want == -2) return fromSnapshot;
    if (p.want == fromSnapshot) { p.want = -2; return fromSnapshot; }
    // Give up after a couple of seconds. A latch that waits forever for a value
    // the game thread never received - because the command was dropped, or was
    // refused as out of range - shows the user a setting that is not real, and
    // the only way out was to change the same control again.
    if (ImGui::GetTime() - p.since > 2.0) { p.want = -2; return fromSnapshot; }
    return p.want;
}

// Post a change and latch it, but only if it actually made it into the queue.
static void PostLatched(Pending& p, gui::CmdType t, int a) {
    if (!Post(t, a)) return;                 // dropped; do not pretend it took
    p.want = a;
    p.since = ImGui::GetTime();
}

using NameFn = const char* (*)(int);
static int  g_settingTrack = -1;         // bound before SettingName is used
static const char* SettingName(int j) { return catalog::Setting(g_settingTrack, j); }

// Returns the newly chosen index, or -2 for "unchanged". -1 means "(default)".
static int NamedCombo(const char* label, int count, NameFn name, int sel,
                      const char* noneLabel) {
    const char* preview = (sel >= 0 && sel < count) ? name(sel)
                        : (noneLabel ? noneLabel : "(not set)");
    int picked = -2;
    // A fixed width, not -1: stretching to the window edge leaves no room for the
    // label, which then sits outside the window and is invisible. 260px fits the
    // longest asset name we have.
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(avail < 300.f ? avail * 0.6f : 260.f);
    if (ImGui::BeginCombo(label, preview)) {
        if (noneLabel && ImGui::Selectable(noneLabel, sel < 0)) picked = -1;
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(name(i), i == sel)) picked = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return picked;
}

// The four identity dropdowns. Shown only on your own row - the host baking
// somebody else's car is a different feature with different consequences.
static void IdentityEditor(const gui::PlayerRow& r) {
    if (!catalog::Ready()) {
        ImGui::TextColored(kWarn, "Asset lists unavailable: %s", catalog::Error());
        return;
    }
    static Pending pv, pd, pp, pc, pdc;
    int v = Resolve(pv, catalog::IndexOfVehicle(r.vehicle));
    int d = Resolve(pd, catalog::IndexOfDriver(r.driver));
    int a = Resolve(pp, catalog::IndexOfPaint(r.paint));
    int c = Resolve(pc, catalog::IndexOfDecal(r.decal));
    int e = Resolve(pdc, catalog::IndexOfDecalColor(r.paintDetail));

    int n;
    n = NamedCombo("vehicle", catalog::VehicleCount(), catalog::Vehicle, v, "(default)");
    if (n != -2) PostLatched(pv, gui::CMD_SET_VEHICLE, n);
    n = NamedCombo("driver",  catalog::DriverCount(),  catalog::Driver,  d, "(default)");
    if (n != -2) PostLatched(pd, gui::CMD_SET_DRIVER, n);
    n = NamedCombo("paint",   catalog::PaintCount(),   catalog::Paint,   a, "(default)");
    if (n != -2) PostLatched(pp, gui::CMD_SET_PAINT, n);
    n = NamedCombo("decal",   catalog::DecalCount(),   catalog::Decal,   c, "(default)");
    if (n != -2) PostLatched(pc, gui::CMD_SET_DECAL, n);
    n = NamedCombo("decal color", catalog::DecalColorCount(), catalog::DecalColor,
                   e, "(default)");
    if (n != -2) PostLatched(pdc, gui::CMD_SET_DECALCOLOR, n);

    ImGui::TextColored(kDim,
        "Broadcast immediately, but the car you drive changes from the NEXT race -");
    ImGui::TextColored(kDim,
        "models are resolved while the track loads and cannot be swapped after.");
    ImGui::TextColored(kDim, "Saved to bbr2_mp.ini, so it is still your car next launch.");
}

static void SelectedPlayer(const gui::Snapshot& s) {
    if (g_selRow < 0) return;
    const gui::PlayerRow* r = nullptr;
    for (int i = 0; i < s.rows; ++i) if (s.p[i].id == g_selRow) { r = &s.p[i]; break; }
    if (!r) { g_selRow = -1; return; }      // they left while selected

    ImGui::Separator();
    ImGui::Text("Slot %d - %s", r->id, r->name[0] ? r->name : "(unnamed)");

    // Spectate. The race is not paused and you are not handed over to anyone -
    // your car keeps driving on your input, you just are not looking at it.
    {
        const bool watching = (r->entIndex >= 0 && s.spectating == r->entIndex) ||
                              (r->isSelf && s.spectating < 0);
        if (watching) {
            ImGui::TextColored(kGood, r->isSelf ? "Camera is on your car"
                                                : "Watching this player");
            if (!r->isSelf && ImGui::Button("Back to my car")) Post(gui::CMD_SPECTATE, -1);
        } else {
            const char* why = !s.raceLive ? "no race is running"
                                          : "this player has no car in this race";
            if (GatedButton(r->isSelf ? "Back to my car" : "Watch this player",
                            r->canSpectate, why))
                Post(gui::CMD_SPECTATE, r->isSelf ? -1 : r->entIndex);
        }
        if (!r->isSelf && r->entIndex >= 0 && s.spectating == r->entIndex)
            ImGui::TextColored(kDim, "Your own car is still driving - mind the wall.");
    }
    ImGui::Spacing();

    if (r->effects[0] && r->isSelf)
        ImGui::TextColored(kWarn, "affected by: %s", r->effects);

    if (r->isSelf) {
        IdentityEditor(*r);
    } else {
        ImGui::Columns(2, nullptr, false);
        ImGui::Text("vehicle : %s", r->vehicle[0] ? r->vehicle : "(default)");
        ImGui::Text("driver  : %s", r->driver[0]  ? r->driver  : "(default)");
        ImGui::Text("paint   : %s", r->paint[0]   ? r->paint   : "(default)");
        ImGui::Text("decal   : %s%s%s", r->decal[0] ? r->decal : "(default)",
                    r->paintDetail[0] ? " in " : "",
                    r->paintDetail[0] ? r->paintDetail : "");
        ImGui::NextColumn();
        ImGui::Text("status  : %s%s%s", r->loading ? "loading " : "",
                    r->ready ? "ready " : "", r->racing ? "racing" : "");
        if (r->effects[0]) ImGui::TextColored(kWarn, "affected: %s", r->effects);
        ImGui::Text("last tick: %u", r->lastTick);
        if (r->packetMs > 0.f) {
            ImGui::Text("packet  : %.0f ms", r->packetMs);
            ImGui::Text("buffer  : %.0f ms   jitter %.0f ms", r->bufferMs, r->jitterMs);
        } else {
            ImGui::TextColored(kDim, "no state packets yet");
        }
        ImGui::Columns(1);
        ImGui::TextColored(kDim, "Only your own car is yours to change.");
    }
}

static void NothingArrivedHelp(const gui::Snapshot& s) {
    ImGui::TextColored(kBad, "Nothing has arrived on UDP %d.", s.port);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Over the internet, plain UDP does not traverse NAT on its own: the host "
        "needs UDP %d forwarded, and clients need the host's PUBLIC address - not "
        "a 192.168.x.x, and not an address a website reported while a VPN or "
        "Cloudflare WARP was on, because that one belongs to the VPN and nothing "
        "can route back through it. Tailscale or ZeroTier sidesteps all of it.",
        s.port);
    ImGui::Spacing();
    ImGui::TextWrapped("Otherwise: Windows Firewall is blocking Game_x64.exe, or "
                       "nobody has the host's address in [net] host.");
}

static void Controls(const gui::Snapshot& s) {
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 half(w * 0.5f - 4.f, 0.f);

    if (GatedButton("Start race  (F9)", s.canStart, s.cantStartWhy, half))
        Post(gui::CMD_START_RACE);
    ImGui::SameLine();
    // One button, labelled for whatever the state machine will actually do next -
    // releasing the loading screen and dropping the lights are two presses of the
    // same key, and calling them both "GO" was confusing on the first run.
    const char* goLabel = s.loadGateHeld ? "Let everyone in  (F10)"
                                         : "Drop the lights  (F10)";
    if (GatedButton(goLabel, s.canGo, s.cantGoWhy, half))
        Post(gui::CMD_GO);

    if (!s.canStart && s.cantStartWhy[0])
        ImGui::TextColored(kDim, "%s", s.cantStartWhy);
    else if (!s.canGo && s.cantGoWhy[0])
        ImGui::TextColored(kDim, "%s", s.cantGoWhy);

    // A cooldown, not because clicking twice is wrong but because both of these
    // are expensive on the game thread and the engine queues toasts rather than
    // replacing them. Mashing the button used to mean a stack of toasts arriving
    // over the next ten seconds.
    static double lastToast = 0.0, lastDump = 0.0;
    const double now = ImGui::GetTime();
    const bool toastOk = now - lastToast > 1.0;
    const bool dumpOk  = now - lastDump  > 1.0;
    if (GatedButton("Lobby toast  (F7)", toastOk, "wait a moment", half)) {
        lastToast = now; Post(gui::CMD_TOAST_STATUS);
    }
    ImGui::SameLine();
    if (GatedButton("Dump to log  (F8)", dumpOk, "wait a moment", half)) {
        lastDump = now; Post(gui::CMD_DUMP_LOG);
    }

    // Same reasoning as the race settings: on a dedicated server there is no
    // host, and an admin is the one calling races.
    if (s.isAdmin) {
        ImGui::Spacing();
        ImGui::TextColored(kDim, "Where the next race waits for everyone:");
        // The snapshot is up to a tick behind - and during a level load the game
        // thread does not tick at all while this panel keeps drawing. Building
        // the new mask from the snapshot each frame meant a second click
        // recomputed from a stale base and silently threw the first one away, and
        // the box you had just ticked visibly snapped back. So hold what we asked
        // for until the snapshot agrees with it.
        static int pending = -1;
        const int hold = (pending >= 0) ? pending : s.holdMode;
        if (pending >= 0 && pending == s.holdMode) pending = -1;

        bool a = (hold & 1) != 0, b = (hold & 2) != 0, c = (hold & 4) != 0;
        bool changed = false;
        changed |= ImGui::Checkbox("loading screen", &a);
        ImGui::SameLine(); changed |= ImGui::Checkbox("grid", &b);
        ImGui::SameLine(); changed |= ImGui::Checkbox("pre-game", &c);
        if (changed) {
            pending = (a ? 1 : 0) | (b ? 2 : 0) | (c ? 4 : 0);
            Post(gui::CMD_SET_HOLD, pending);
        }
        if (!a && !b && !c)
            ImGui::TextColored(kWarn, "nothing holds - players will start whenever "
                                      "they finish loading");
        ImGui::TextColored(kDim, "Applies to the next race, not the one loading.");
    }
}

static void OverlayPanel() {
    // Keep the last snapshot we managed to read. gui::Read only fails while the
    // game thread is mid-publish, which is microseconds - but blanking the whole
    // panel for one frame when it happens is a visible flicker for no reason.
    static gui::Snapshot last{};
    static bool          haveLast = false;
    gui::Snapshot s{};
    if (gui::Read(s)) { last = s; haveLast = true; }
    else if (haveLast) s = last;
    else { ImGui::TextDisabled("waiting for the game thread..."); return; }

    // --- header ---
    ImGui::Text("%s", s.isHost ? "HOST" : "CLIENT");
    ImGui::SameLine();
    if (s.headlessHost) {
        ImGui::TextColored(s.isAdmin ? kGood : kDim,
                           s.isAdmin ? "on a dedicated server (you are an admin)"
                                     : "on a dedicated server");
        ImGui::SameLine();
    }
    if (s.myId < 0) ImGui::TextColored(kWarn, "- no id yet");
    else            ImGui::TextColored(kDim, "- slot %d of %d", s.myId, s.maxPlayers);
    ImGui::SameLine(0.f, 16.f);
    if (s.raceLive) ImGui::TextColored(kGood, "%s", PhaseName(s.phase));
    else            ImGui::TextColored(kDim,  "%s", PhaseName(s.phase));

    if (s.lobbySize <= 1) {
        ImGui::TextColored(kDim, "Just you so far - nobody else has connected.");
    } else {
        int racing = 0;
        for (int i = 0; i < s.rows; ++i) if (s.p[i].racing) ++racing;
        ImGui::Text("You + %d other%s", s.lobbySize - 1, s.lobbySize == 2 ? "" : "s");
        ImGui::SameLine();
        if (s.raceLive) ImGui::TextColored(kDim, "- %d racing", racing);
        else            ImGui::TextColored(kDim, "- %d ready", s.readyCount);
    }

    if (s.spectating >= 0) {
        const char* who = (s.spectating < s.carCount) ? s.cars[s.spectating].name : "another car";
        ImGui::TextColored(kWarn, "Camera is on %s - your car is still yours to drive.", who);
        ImGui::SameLine();
        if (ImGui::SmallButton("back")) Post(gui::CMD_SPECTATE, -1);
    }

    ImGui::Separator();
    PlayerTable(s);
    SelectedPlayer(s);
    ImGui::Separator();
    Controls(s);
    ImGui::Separator();

    // The race the ini describes - what pressing Start would load. This is the
    // honest answer to "the lobby numbers look wrong": until a race is running,
    // the engine's own config holds whatever the front end last put in it, and
    // reading that back was never going to show your settings.
    if (ImGui::CollapsingHeader("Next race", ImGuiTreeNodeFlags_DefaultOpen)) {
        // isAdmin, NOT isHost. On a dedicated server nobody is the host, and the
        // command handlers have always used IsAdmin() - only this panel still
        // asked the narrower question, so an admin could press Start but could
        // not see or change what they were starting.
        if (!s.isAdmin) {
            // A plain client changing these would only desync it from the race it
            // is about to be told to load, so show them read-only.
            ImGui::TextColored(kDim, s.headlessHost
                ? "The server's admins pick the race. Yours would be ignored."
                : "The host picks the race. Yours would be ignored.");
            ImGui::Text("track    : %s", s.wantTrack[0] ? s.wantTrack : "(not set)");
            ImGui::Text("setting  : %s", s.wantSetting[0] ? s.wantSetting : "(track default)");
            ImGui::Text("mode     : %s", s.wantGameType[0] ? s.wantGameType : "Race");
            ImGui::Text("laps     : %d", s.wantLaps);
        } else if (!catalog::Ready()) {
            ImGui::TextColored(kWarn, "Asset lists unavailable: %s", catalog::Error());
            ImGui::Text("track    : %s", s.wantTrack[0] ? s.wantTrack : "(not set)");
            ImGui::Text("setting  : %s", s.wantSetting[0] ? s.wantSetting : "(track default)");
            ImGui::TextColored(kDim, "bbr2_mp.ini still works - it always does.");
        } else {
            static Pending pt, ps, pg;
            const int t = Resolve(pt, catalog::IndexOfTrack(s.wantTrack));
            int n = NamedCombo("track", catalog::TrackCount(), catalog::Track, t, nullptr);
            if (n != -2) { PostLatched(pt, gui::CMD_SET_TRACK, n); ps.want = -2; }

            // Settings belong to a track: CastleA has BlueSkyB, NightB, SunsetB
            // and nothing else. Offering all 82 combinations would mostly offer
            // ones that do not exist.
            // Re-resolve: if the track just changed, the settings below belong to
            // the NEW track, not the one this frame started with.
            const int tNow = Resolve(pt, catalog::IndexOfTrack(s.wantTrack));
            g_settingTrack = tNow;
            const int nSet = catalog::SettingCount(tNow);
            if (nSet > 0) {
                const int sel = Resolve(ps, catalog::IndexOfSetting(tNow, s.wantSetting));
                n = NamedCombo("setting", nSet, SettingName, sel, "(track default)");
                if (n != -2) PostLatched(ps, gui::CMD_SET_SETTING, n);
            } else {
                ImGui::TextColored(kDim, "setting  : this track has no variants");
            }

            const int g = Resolve(pg, catalog::IndexOfGameType(s.wantGameType));
            n = NamedCombo("mode", catalog::GameTypeCount(), catalog::GameType, g, nullptr);
            if (n != -2) PostLatched(pg, gui::CMD_SET_GAMETYPE, n);

            static Pending pl, pgr;
            int laps = Resolve(pl, s.wantLaps);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::InputInt("laps", &laps)) {
                if (laps < 1) laps = 1;
                if (laps > 99) laps = 99;
                PostLatched(pl, gui::CMD_SET_LAPS, laps);
            }
            int grid = Resolve(pgr, s.wantGrid);
            ImGui::SetNextItemWidth(120.f);
            if (ImGui::InputInt("grid (0 = auto)", &grid)) {
                if (grid < 0) grid = 0;
                if (grid > 32) grid = 32;
                PostLatched(pgr, gui::CMD_SET_GRID, grid);
            }
            static Pending pm;
            bool mirror = Resolve(pm, s.wantMirror ? 1 : 0) != 0;
            if (ImGui::Checkbox("mirrored", &mirror))
                PostLatched(pm, gui::CMD_SET_MIRROR, mirror ? 1 : 0);

            ImGui::Spacing();
            if (ImGui::Button("Save to bbr2_mp.ini")) Post(gui::CMD_SAVE_INI);
            ImGui::SameLine();
            ImGui::TextColored(kDim, "%s", catalog::SourceSummary());
        }
    }

    // Every car on the grid, not just the networked ones. This is also how you
    // test spectating with nobody else connected: start a race and watch an AI.
    if (ImGui::CollapsingHeader("Cameras")) {
        if (!s.raceLive) {
            ImGui::TextColored(kDim, "No race running - nothing to point a camera at.");
        } else {
            if (GatedButton("Back to my car", s.spectating >= 0, "already on your car"))
                Post(gui::CMD_SPECTATE, -1);
            ImGui::Spacing();
            for (int i = 0; i < s.carCount; ++i) {
                const gui::Snapshot::Car& car = s.cars[i];
                ImGui::PushID(1000 + i);
                const bool on = car.isMe ? (s.spectating < 0) : (s.spectating == i);
                if (on) ImGui::TextColored(kGood, "* ");
                else    ImGui::TextUnformatted("  ");
                ImGui::SameLine(0.f, 0.f);
                if (ImGui::SmallButton("watch")) Post(gui::CMD_SPECTATE, car.isMe ? -1 : i);
                ImGui::SameLine();
                ImGui::Text("%2d  %-18s", i, car.name);
                ImGui::SameLine();
                if (car.isMe)            ImGui::TextColored(kGood, "you");
                else if (car.peerId >= 0)ImGui::TextColored(kGood, "player %d", car.peerId);
                else if (car.isAi)       ImGui::TextColored(kDim,  "AI");
                if (car.vehicle[0]) { ImGui::SameLine(); ImGui::TextColored(kDim, "%s", car.vehicle); }
                ImGui::PopID();
            }
        }
    }

    if (ImGui::CollapsingHeader("Live in the engine")) {
        if (!s.raceLive)
            ImGui::TextColored(kDim, "No race running. These are whatever the front "
                                     "end last left in VuGameConfig, not your settings.");
        ImGui::Text("track    : %s", s.track[0] ? s.track : "-");
        ImGui::Text("setting  : %s", s.setting[0] ? s.setting : "-");
        ImGui::Text("mode     : %s (%d)", s.gameTypeName[0] ? s.gameTypeName : "?",
                    s.gameType);
        ImGui::Text("laps     : %d", s.lapCount);
        if (s.raceLive && s.askedLaps && s.askedLaps != s.lapCount) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "(we asked for %d - the engine overrode it)",
                               s.askedLaps);
        }
        if (s.raceLive && s.askedTrack[0] && s.track[0] &&
            _stricmp(s.askedTrack, s.track) != 0)
            ImGui::TextColored(kWarn, "asked for track \"%s\"", s.askedTrack);
        ImGui::Text("race id  : %u", s.raceId);
        ImGui::Text("gates    : countdown %s, pre-game %s, start %s, load %s",
                    s.countdownHeld ? "HELD" : "open",
                    s.preGameHeld   ? "HELD" : "open",
                    s.startGateHeld ? "HELD" : "open",
                    s.loadGateHeld  ? "HELD" : "open");
    }

    if (ImGui::CollapsingHeader("Network")) {
        ImGui::Text("UDP port : %d   (protocol v%d)", s.port, s.protocol);
        if (!s.isHost)
            ImGui::Text("host at  : %s", s.hostAddr[0] ? s.hostAddr : "UNKNOWN");
        ImGui::Text("sent     : %u packets", s.txTotal);
        if (s.isHost) ImGui::Text("relayed  : %u", s.relayed);
        ImGui::Text("received : %u  (good %u)", s.rxTotal, s.rxGood);
        if (s.rxJunk || s.rxBadVersion || s.rxSelf)
            ImGui::TextColored(kWarn, "discarded: junk %u, wrong version %u, own id %u",
                               s.rxJunk, s.rxBadVersion, s.rxSelf);
        if (s.rxTotal == 0) { ImGui::Spacing(); NothingArrivedHelp(s); }

        // Hit claims, on their own line because they are the one thing here whose
        // absence is invisible in every other number. "sent" climbing with
        // "received" flat on the other machine is the whole diagnosis.
        ImGui::Spacing();
        ImGui::Text("hit claims sent : %u   (each goes out 5x)", s.hitsSent);
        ImGui::Text("hit claims recvd: %u   (repeats swallowed: %u)",
                    s.hitsRecv, s.hitsDupes);
        if (s.isHost)
            ImGui::Text("relayed to victim: %u", s.hitsRelayed);
        if (s.hitsRefused || s.hitsUndeliverable || s.hitsDropped)
            ImGui::TextColored(kWarn,
                "refused (bad token) %u, undeliverable %u, repeats dropped %u",
                s.hitsRefused, s.hitsUndeliverable, s.hitsDropped);
        if (s.hitsSent == 0 && s.raceLive)
            ImGui::TextDisabled("nothing claimed yet - fire a power-up at somebody");

        // The rules in force, and whose they are. Worth the four lines: "why did
        // that not hit me" is nearly always answered by a number somebody else
        // chose, and until v8 there was no way to see it from inside the game.
        ImGui::Spacing();
        ImGui::TextDisabled("rules for this race - set by %s",
                            s.rulesFromHost ? "the host/server" : "your own ini");
        if (!s.rulesEffects) {
            ImGui::TextColored(kWarn, "effect replication is OFF for this race");
        } else {
            ImGui::Text("claims %s | grace %.1fs, timeout %.1fs",
                        s.rulesClaims ? "ON" : "off", s.rulesGrace, s.rulesTimeout);
            ImGui::Text("cooldown %.1fs between claims of one effect", s.rulesCooldown);
            ImGui::Text("refuse past %.1fs old or %.0f m of drift",
                        s.rulesMaxAge, s.rulesMaxDrift);
        }
    }

    if (ImGui::CollapsingHeader("Mod")) {
        ImGui::Text("uptime   : %.0f s", s.uptime);
        ImGui::Text("game tick: %u  (%.1f ms)", s.gameTicks, s.dt * 1000.f);
        ImGui::Text("render   : %.0f fps", ImGui::GetIO().Framerate);
    }
}

// ---- lobby + apply (Network phase, game thread) ---------------------------
// ---- shooter authority, receiving end ------------------------------------
//
// Somebody says their shot landed on us. We are the only machine that may put an
// effect on our own car, so this is where a claim turns into a real effect - and
// once it is on, our ordinary state packet carries it and every machine in the
// session, including theirs, renders the same answer.
//
// Shooter authority does NOT mean the shooter is always right. It means the
// shooter's VIEW is the starting point, and the victim checks whether that view
// was close enough to reality to count. This is where that check lives, and it is
// the whole difference between "what you saw happens" and "a player at 500 ms with
// heavy loss can freeze anybody still on his screen".
//
// THE PLAUSIBILITY TEST. Everyone aims at a ghost: remote cars are drawn a playout
// buffer behind real time, 60-150 ms normally and worse on a bad link. So every
// shot is fired at where the target WAS, and some staleness has to be forgiven or
// the grazing hit at speed - the exact shot this feature exists for - never lands.
//
// The right question is not "how old is this claim" but "how far have I moved
// since the picture they were shooting at". Those come apart precisely where it
// matters: half a second of lag against a stopped car is harmless and the hit is
// real, while half a second against a car at 40 m/s is a shot at a corner that car
// left twenty metres ago. So we keep a short ring of where we have actually been,
// and the shooter tells us WHICH MOMENT of it they were looking at.
//
// That last part is what makes this exact rather than estimated. `victimViewUs` is
// the shooter's interpolator playback position for our car - our own race clock,
// our own units, no conversion, no latency estimate, no clock-offset guess. An
// earlier attempt sent the shooter's clock and converted with a measured per-peer
// offset; it silently passed everything, because such an offset necessarily
// contains one-way transit, so the conversion always lands on "now" and the
// measured drift is always zero. The lesson is worth keeping: if a gate reports
// zero every time, it is not lenient, it is broken.
//
// The other tests, in cost order: right race; not a multi-second backlog; not
// already on us (both machines fire, so our own copy of their shot has usually
// landed - and without this two players could chain someone indefinitely); and
// nameable in VehicleEffectDB.
static void DrainHits() {
    net::HitEvent ev{};
    while (net::TakeHit(ev)) {
        // Drained even when the feature is off, so turning it off does not leave
        // a queue quietly filling behind it.
        if (!g_rules.hitClaims || !g_rules.syncEffects) continue;
        if (!g_raceLive || g_raceId == 0 || ev.raceId != g_raceId) continue;
        if (ev.attacker >= net::kMaxPeers) continue;

        VehicleEntity me = LocalPlayerEntity();
        VehicleState mine{};
        if (!me || !CaptureState(me, mine)) continue;

        net::Peer* peer = net::GetPeer(ev.attacker);
        const char* who = (peer && peer->name[0]) ? peer->name : "someone";

        // The moment of us they were aiming at, on OUR clock.
        const float viewT   = (float)ev.victimViewUs * 1e-6f;
        const float viewAge = g_raceTime - viewT;

        if (viewAge > g_rules.hitMaxAge) {
            if (g_moanAge++ < 20)
                Log("[fx] refused a hit from %s: they were looking at us %.1f s ago "
                    "(hitmaxage %.1f s) - a backlog, not a shot",
                    who, viewAge, g_rules.hitMaxAge);
            continue;
        }

        // How far we have travelled since that picture was taken. THE test.
        float then[3];
        if (!WhereWeWere(viewT, then)) {
            // No history to judge against - the opening frames of a race, or a
            // claim about a moment we have already forgotten. Refuse: a gate with
            // no evidence must not default to yes.
            if (g_moanNoHist++ < 10)
                Log("[fx] refused a hit from %s: no record of where we were at "
                    "%.2f s into the race", who, viewT);
            continue;
        }
        const float dx = mine.pos[0] - then[0];
        const float dy = mine.pos[1] - then[1];
        const float dz = mine.pos[2] - then[2];
        const float drift = sqrtf(dx*dx + dy*dy + dz*dz);
        if (drift > g_rules.hitMaxDrift) {
            if (g_moanDrift++ < 20)
                Log("[fx] refused a hit from %s: we have moved %.1f m since the "
                    "moment they were shooting at (limit %.1f m) - they were aiming "
                    "at where we used to be", who, drift, g_rules.hitMaxDrift);
            continue;
        }

        const char* nm = catalog::EffectName(ev.nameHash);
        if (!nm[0]) {
            Log("[fx] %s claims effect %08x, which is not in VehicleEffectDB "
                "- ignoring", who, ev.nameHash);
            continue;
        }

        ActiveEffect on[24];
        const int nOn = ReadVehicleEffects(me, on, 24);
        bool already = false;
        for (int a = 0; a < nOn; ++a)
            if (on[a].nameHash == ev.nameHash) { already = true; break; }
        if (already) continue;   // our own copy of their shot already landed

        // The measured drift is reported on accepted hits as well as refused ones,
        // so hitmaxdrift can be set from real numbers: if the hits everyone agrees
        // were fair keep reading 12 m, the limit is too tight for how you play.
        // WITH the attacker's vehicle, not the plain overload - which explicitly
        // zeroes the originator, making a replicated hit indistinguishable from a
        // puddle and silently suppressing the engine's own hit notification. Pass
        // it and the victim's screen says who hit them, correctly, for free.
        // Resolved HERE, not taken from the cached rp.ent. DrainHits runs at the
        // top of OnTick and that cache is refreshed at the bottom, so it is always
        // a frame stale - and staler than that if the peer left, if the slot was
        // handed to somebody new (the refresh is skipped on an ownerHash mismatch,
        // so ent keeps pointing at the previous occupant's car), or if this is the
        // first frame of the race. The originator is a raw pointer the engine may
        // dereference, so it gets the same isLive() check every other consumer in
        // the SDK does.
        VehicleEntity byWhom{};
        {
            RemotePlayer& ap = g_rp[ev.attacker];
            net::Peer* apeer = net::GetPeer(ev.attacker);
            const bool sameHuman = ap.ownerHash == 0 ||
                (apeer && ap.ownerHash == NameHash(apeer->name));
            if (ap.inThisRace && !ap.gone && ap.cfg && sameHuman) {
                VehicleEntity e = EntityForConfigVehicle(ap.cfg);
                if (e && e.isLive()) byWhom = e;
            }
        }
        const int lvl = (ev.level <= 12) ? (int)ev.level : 0;
        if (ApplyVehicleEffectFrom(me, nm, lvl, byWhom))
            Log("[fx] %s hit us with %s - applied (aimed %.0f ms back, %.1f m of "
                "drift)", who, nm, viewAge * 1000.f, drift);
        else
            Log("[fx] %s hit us with %s - the engine refused it (shielded, "
                "respawning, or the DB says do not reapply)", who, nm);
    }
}


// Watch our own car for a power-up going off, and tell everybody.
//
// Detected by the COUNT going down rather than by the button, because the button
// is not usable: it is a strict one-frame rising edge written inside the input
// tick, so any sampler misses most presses. The count is the outcome, and the
// outcome is what the others have to reproduce.
static void WatchOwnPowerUps() {
    if (!g_rules.syncPowerUps || !g_raceLive || g_raceId == 0 || !HaveId()) {
        g_ownSlotInit = false;
        return;
    }
    VehicleEntity me = LocalPlayerEntity();
    if (!me || !me.isLive()) { g_ownSlotInit = false; return; }

    OwnSlot now[3]{};
    PowerUpSlot held[2];
    ReadPowerUpSlots(me, held);
    for (int i = 0; i < 2; ++i) {
        now[i].pu = held[i].powerUp; now[i].level = held[i].level;
        now[i].count = held[i].count;
    }
    int abilityCount = 0, abilityLevel = 0;
    now[2].pu = ReadAbilityPowerUp(me, &abilityCount, &abilityLevel);
    now[2].count = abilityCount;
    // The ability's level field is inferred from the slot layout's stride and is
    // NOT confirmed by disassembly, so it is clamped rather than trusted. A wrong
    // level costs the wrong duration; it cannot corrupt anything, because
    // applyEffect clamps again on the way in.
    now[2].level = (abilityLevel >= 0 && abilityLevel <= 12) ? abilityLevel : 0;

    if (g_ownSlotInit) {
        for (int i = 0; i < 3; ++i) {
            // A use is the count going DOWN on a slot still holding the same card.
            // Collecting only ever raises it, and a card swap is a collection.
            const bool sameCard = g_ownSlot[i].pu && g_ownSlot[i].pu == now[i].pu;
            const bool swapped  = g_ownSlot[i].pu && now[i].pu && g_ownSlot[i].pu != now[i].pu;
            const bool emptied  = g_ownSlot[i].pu && !now[i].pu && g_ownSlot[i].count > 0;
            int fired = 0;
            if (sameCard && now[i].count < g_ownSlot[i].count)
                fired = g_ownSlot[i].count - now[i].count;
            else if (emptied)
                fired = 1;                  // last charge: the slot nulls itself
            else if (swapped)
                // Used the last charge AND drove through a bubble in the same frame,
                // so the slot already holds a different card by the time we look.
                // Without this the use vanishes and nobody sees the shot - and
                // driving over a pickup as you fire is an ordinary racing line.
                fired = 1;
            if (fired > 4) fired = 4;       // corruption backstop, not an expected bound
            for (int k = 0; k < fired; ++k) {
                const uint32_t h = catalog::PowerUpHashOf(g_ownSlot[i].pu);
                if (!h) {
                    static int moaned = 0;
                    if (moaned++ < 5)
                        Log("[pu] we used something we cannot name - nobody else "
                            "will see it. Is the PowerUps sheet complete?");
                    continue;
                }
                net::SendUse((uint8_t)i, (uint8_t)g_ownSlot[i].level, h,
                             ++g_useEventId, g_raceId);
                Log("[pu] we used %s (slot %d, level %d) - use #%u sent",
                    catalog::PowerUpNameOf(h), i, g_ownSlot[i].level, g_useEventId);
            }
        }
    }
    for (int i = 0; i < 3; ++i) g_ownSlot[i] = now[i];
    g_ownSlotInit = true;
}

// The other end: somebody used one, so run the engine's own use path on our copy
// of their car. That is what spawns the swarm, drops the obstacle and launches the
// missile - none of which any amount of effect-list mirroring can produce, because
// none of them live on a car.
static void DrainUses() {
    net::UseEvent ev{};
    while (net::TakeUse(ev)) {
        if (!g_rules.syncPowerUps) continue;
        if (!g_raceLive || g_raceId == 0 || ev.raceId != g_raceId) continue;
        if (ev.player >= net::kMaxPeers || !HaveId() || ev.player == (uint8_t)MyId()) continue;
        if (ev.slot > 2) continue;

        RemotePlayer& rp = g_rp[ev.player];
        if (!rp.inThisRace || rp.gone || !rp.ent || !rp.ent.isLive()) continue;
        net::Peer* p = net::GetPeer(ev.player);
        if (rp.ownerHash && (!p || rp.ownerHash != NameHash(p->name))) continue;

        const void* pu = catalog::PowerUpFromHash(ev.powerUpHash);
        if (!pu) {
            static int moaned = 0;
            if (moaned++ < 5)
                Log("[pu] slot %u used power-up %08x, which we cannot resolve - "
                    "nothing will appear for it here", ev.player, ev.powerUpHash);
            continue;
        }
        const char* who = (p && p->name[0]) ? p->name : "someone";
        if (UsePowerUpAs(rp.ent, (int)ev.slot, pu, (int)ev.level))
            Log("[pu] %s used %s - fired it on our copy of their car",
                who, catalog::PowerUpNameOf(ev.powerUpHash));
        else
            Log("[pu] %s used %s - the engine refused it on our copy",
                who, catalog::PowerUpNameOf(ev.powerUpHash));
    }
}


// ---- finishing --------------------------------------------------------------
//
// The engine ends the race the frame after the LOCAL driver crosses the line -
// its end condition is "no vehicle WITH A VIEWPORT is still unfinished", and a
// remote car has no viewport. finishAllVehicles then invents everybody else's
// time by extrapolating from distance covered and DNF-stamps anything past the
// leader plus the DNF window. So whoever gets home first sees DNF against every
// human in the race, permanently, however they actually did.
//
// The fix is to tell everyone when we cross, and to write their real times into
// our own results when theirs arrive. That works even after the screen is up: the
// results table re-reads and re-sorts its fields every frame.

// Let the ENGINE renumber, from the times we have just corrected.
//
// Numbering the humans 1..n by hand was wrong: place is a global rank across the
// whole grid, so it overwrote the numbers the AI cars were holding, and the
// results table - which sorts on place, then score, then time - silently
// reordered on the ties. A human who finished last could be shown third.
static void RestackPlaces() { RenumberPlaces(); }

// Watch our own car for the finish flag and say so once.
static void WatchOwnFinish() {
    if (!g_raceLive || g_raceId == 0 || !HaveId() || g_sentFinish) return;
    VehicleEntity me = LocalPlayerEntity();
    RaceProgress rp{};
    if (!ReadRaceProgress(me, rp) || !rp.finished) return;
    if (rp.dnf) {
        // Force-finished rather than actually finished - the time on the car is an
        // extrapolation, not a lap. Broadcasting it would overwrite everyone
        // else's correct DNF for us with an invented result.
        g_sentFinish = true;
        Log("[fin] we were DNF'd rather than finishing - not claiming a time");
        return;
    }
    g_sentFinish = true;
    const uint32_t ms = (uint32_t)(rp.timeSec * 1000.0);
    const int slot = MyId();
    if (slot < net::kMaxPeers) { g_finishMs[slot] = ms; g_haveFinish[slot] = true; }
    net::SendFinish(g_raceId, ms, (uint8_t)(rp.lap < 0 ? 0 : rp.lap > 255 ? 255 : rp.lap),
                    ++g_finishEventId);
    Log("[fin] we finished at %.2fs on lap %d - told everyone", rp.timeSec, rp.lap);
    RestackPlaces();
}

// ...and write everyone else's in when it arrives.
static void DrainFinishes() {
    net::FinishEvent ev{};
    while (net::TakeFinish(ev)) {
        if (g_raceId == 0 || ev.raceId != g_raceId) continue;
        if (ev.player >= net::kMaxPeers) continue;
        RemotePlayer& rp = g_rp[ev.player];
        // The slot may have been handed to somebody new mid-race. Their finish must
        // not be written onto the car the previous occupant abandoned - that would
        // clear a genuine DNF and stamp a time nobody earned onto the results.
        net::Peer* pr = net::GetPeer(ev.player);
        if (rp.ownerHash && (!pr || rp.ownerHash != NameHash(pr->name))) continue;
        VehicleEntity v = (rp.inThisRace && rp.cfg) ? EntityForConfigVehicle(rp.cfg)
                                                    : VehicleEntity{};
        if (!v || !v.isLive()) {
            // Their car is gone - nothing to correct, but remember the time so the
            // places still come out right for everyone who does have one.
            g_finishMs[ev.player] = ev.timeMs; g_haveFinish[ev.player] = true;
            continue;
        }
        WriteRaceFinish(v, (double)ev.timeMs / 1000.0, (int)ev.lap);
        g_finishMs[ev.player] = ev.timeMs; g_haveFinish[ev.player] = true;
        net::Peer* p = net::GetPeer(ev.player);
        Log("[fin] %s finished at %.2fs - DNF cleared and their time written in",
            (p && p->name[0]) ? p->name : "someone", ev.timeMs / 1000.0);
        RestackPlaces();
    }
}

// Lowest live player id wins. It needs no election, it is the same answer on
// every machine, and it survives anyone but that player leaving. If we ARE that
// player we use our own bucket; otherwise we use the one riding in their state.
static void SyncBubbleStage() {
    if (!g_raceLive || !HaveId()) { SetBubbleStage(-1); SetBubbleEpoch(0); return; }
    const int me = MyId();
    int auth = -1;
    for (int i = 0; i < net::kMaxPeers; ++i) {
        net::Peer* p = net::GetPeer(i);
        if (i == me || (p && net::PeerAlive(i) && p->helloSeen)) { auth = i; break; }
    }
    if (auth < 0 || auth == me) {
        SetBubbleStage(RaceStage());
        SetBubbleEpoch((uint32_t)(g_raceTime / kBubbleEpochSec) & 0xffu);
        return;
    }
    net::Peer* p = net::GetPeer(auth);
    if (!p) { SetBubbleStage(-1); return; }
    SetBubbleStage((int)((p->state.flags >> 1) & 3u));
    SetBubbleEpoch((p->state.flags >> 3) & 0xffu);
}

static void OnTick(float dt, void*) {
    NotifyTick(dt);
    TickAutoSave(dt);
    // Pause is a ref-count on VuTickManager and the tick zeroes the frame delta
    // while it is set. Nobody else in a race can pause with you, so during a
    // networked race we set the engine's own "tick anyway" bypass and let the
    // menu open over a world that keeps running. Applied every frame rather than
    // on the way in: it costs one byte-write, it cannot be left set by a crash
    // path, and it is already correct on the frame the player presses Escape.
    {
        const bool want = !g_cfg.pauseFreeze && g_raceLive && net::Ready() && HaveId();
        static int said = -1;
        if ((int)want != said) {
            said = (int)want;
            if (want) Log("[race] pause will not freeze the world for this race "
                          "(set [race] pausefreeze = true for the stock behaviour)");
            else      Log("[race] pause freezes the world again");
        }
        SetPauseBypass(want);
    }
    if (g_gridReportAt >= 0.f) {
        g_gridReportAt += dt;
        if (g_gridReportAt > 2.0f) { g_gridReportAt = -1.f; GridReport("grid 2s in"); }
    }
    // The asset lists come out of the engine's own tables, which are not loaded
    // yet when ModMain runs. Keep asking; it gives up on its own after ~30s.
    // Not just for the overlay. Effect replication needs the hash->name table out
    // of VehicleEffectDB to apply anything at all, so turning the overlay off used
    // to silently disable every hit and every mirrored effect - with a log line
    // that pointed at the game's data files rather than at the setting the player
    // had just changed.
    // Not just for the overlay, and not just until Ready(): VehicleEffectDB is a
    // separate asset that can finish loading after the spreadsheets, and without
    // its hash->name table every incoming hit is refused as "not in
    // VehicleEffectDB" for the rest of the session.
    if (g_cfg.guiEnabled || g_cfg.syncEffects || g_rules.syncEffects ||
        g_cfg.syncPowerUps || g_rules.syncPowerUps) {
        if (!catalog::Ready() || catalog::EffectCount() == 0) catalog::Build();
    }
    PollHotkeys();
    DrainGuiCommands();
    TickSpectate();
    PublishGui(dt);
    if (!net::Ready()) return;
    net::Poll();
    DrainHits();
    DrainUses();
    DrainFinishes();
    WatchOwnPowerUps();
    WatchOwnFinish();
    SyncBubbleStage();
    g_phaseAcc += dt;
    g_endAcc   += dt;

    // Heartbeat. HELLO used to be sent twice in a session; losing the startup one
    // left the host with no peers.
    static float helloAcc = 0.f;
    helloAcc += dt;
    if (helloAcc > 2.0f) { helloAcc = 0.f; if (!g_raceLive) Hello(); }

    // Greet the player once we know who we are.
    //
    // HaveId() is the whole test, and it is enough on its own: an id is only ever
    // handed out in a WELCOME, so having one already proves the round trip works.
    // This used to also require LobbySize() > 1, from when "client" implied "there
    // is a host, and the host is a player" - so being alone meant nothing had
    // connected yet. On a dedicated server the server is NOT a player, so the
    // first person to join sees a lobby of exactly one, and was never greeted at
    // all. The host branch was unaffected, which is why it looked like only the
    // host message worked.
    static bool greeted = false;
    if (!greeted && HaveId()) {
        greeted = true;
        char body[160];
        if (net::IsHost())
            sprintf_s(body, "Hi %s - you are the host. F9 starts a race, F6 opens the panel",
                      g_cfg.name);
        else if (net::ServerFlags() & net::SF_HEADLESS)
            sprintf_s(body, "Hi %s - player %d on a dedicated server. %s",
                      g_cfg.name, MyId(),
                      net::IsAdmin() ? "F9 asks it to start a race, F10 drops the lights"
                                     : "An admin starts the race. F6 opens the panel");
        else
            sprintf_s(body, "Hi %s - connected as player %d. The host starts the race",
                      g_cfg.name, MyId());
        Notify(body);
        Log("[mp] %s", body);
    }

    // Announce arrivals and departures. Every machine sees them, because the host
    // relays the roster - so a client learns about other clients too.
    {
        static bool  wasHere[net::kMaxPeers] = {};
        static bool  rosterPrimed = false;
        static float primeAcc = 0.f;
        // Joining a busy server used to fire one "X joined" toast per person
        // already there - they all transition not-seen -> seen on our first
        // roster. Nobody joined; we did. Let the roster settle, adopt everyone in
        // it as already-present, and say how many there are exactly once.
        if (!rosterPrimed && HaveId()) {
            primeAcc += dt;
            if (primeAcc >= 1.5f) {
                rosterPrimed = true;
                for (int i = 0; i < net::kMaxPeers; ++i) {
                    if (i == MyId()) continue;
                    net::Peer* p = net::GetPeer(i);
                    if (p && net::PeerAlive(i) && p->helloSeen) wasHere[i] = true;
                }
                const int total = LobbySize();
                if (total > 1) {
                    char body[96];
                    sprintf_s(body, "%d people are online. F6 for info", total);
                    Notify(body);
                    Log("[mp] %s", body);
                }
            }
        }
        for (int i = 0; i < net::kMaxPeers; ++i) {
            if (i == MyId()) continue;
            net::Peer* p = net::GetPeer(i);
            const bool here = p && net::PeerAlive(i) && p->helloSeen;
            if (here == wasHere[i]) continue;
            wasHere[i] = here;
            // Snapshot the name FIRST. On a departure the peer record has usually
            // already been blanked by the timeout, and the branches below clear
            // g_rp[i] outright - so by the time the message was built there was
            // nothing left to print and it read "left (you + 1 other total)"
            // with no name at all.
            char nameBuf[32];
            const char* src = (p && p->name[0]) ? p->name
                            : (g_rp[i].ownerName[0] ? g_rp[i].ownerName : "");
            if (src[0]) strncpy_s(nameBuf, src, sizeof(nameBuf) - 1);
            else        sprintf_s(nameBuf, "Player %d", i);
            if (!here) {
                // Their car cannot be removed from a running race - it is one of
                // VuBaseGame's entities and the standings reference it - so mark
                // it instead, while we still have the handle.
                //
                // Guarded on cfg, NOT on g_raceLive: cfg is non-null exactly
                // between PreBegin and PreEnd, and g_raceLive only goes true in
                // PostBegin. Quitting during the level load is common, and using
                // g_raceLive missed precisely that window - leaving the car to
                // race the whole event under the engine's own AI with the
                // departed player's name on it.
                if (g_rp[i].cfg) {
                    char label[96];
                    const char* nm = g_rp[i].ownerName[0] ? g_rp[i].ownerName
                                   : ((p && p->name[0]) ? p->name : "Player");
                    sprintf_s(label, "%s %s[DISCONNECTED]", nm, kGrey);
                    SetVehicleName(g_rp[i].cfg, label);
                    g_ghostCar[i] = true;      // and stop driving it
                    // Keep cfg/ownerHash so the SAME player can be re-adopted if
                    // this was a stall rather than a quit. Everything that would
                    // otherwise leak into a newcomer is cleared below.
                    g_rp[i].gone = true;
                    g_rp[i].ent = VehicleEntity{};
                    g_rp[i].sync = RemoteSync{};
                    g_rp[i].sync.delayScale = g_cfg.interpDelay;
                    g_rp[i].appliedTick = 0;
                    g_rp[i].stateAge = 0.f;
                    // The wreck branch has just stripped every effect off the car,
                    // so every one of these is stale. Left set, they make the
                    // re-adopted car claim and strip against a world that no longer
                    // exists. effectsStripped is cleared so a SECOND disconnect in
                    // the same race still cleans up.
                    g_rp[i].ghostInit = false; g_rp[i].ghostCount = 0;
                    g_rp[i].pendingCount = 0;  g_rp[i].coolCount = 0;
                    g_rp[i].tallyCount = 0;
                    g_rp[i].effectsStripped = false;
                } else {
                    // No car of theirs in this race - nothing to leave behind.
                    g_rp[i] = RemotePlayer{};
                }
            } else if (g_rp[i].gone) {
                // They are back. Only re-adopt if it is the SAME person: an
                // 8-second gap and a genuine quit look identical from here, and
                // the slot may since have been handed to somebody else entirely.
                if (p && g_rp[i].ownerHash == NameHash(p->name)) {
                    g_ghostCar[i] = false;
                    g_rp[i].gone = false;
                    g_rp[i].sync = RemoteSync{};
                    g_rp[i].sync.delayScale = g_cfg.interpDelay;
                    g_rp[i].appliedTick = 0;
                    g_rp[i].shownName[0] = 0;      // force the label back
                    // Everything the effect machinery remembers about them is now
                    // stale: the wreck branch stripped the car bare while they were
                    // away, and their clock offset was measured before the gap.
                    // Diffing against any of it produces a burst of false claims on
                    // the frame they return.
                    g_rp[i].ghostInit = false; g_rp[i].ghostCount = 0;
                    g_rp[i].pendingCount = 0;  g_rp[i].coolCount = 0;
                    g_rp[i].tallyCount = 0;
                    g_rp[i].effectsStripped = false;
                    Log("[mp] slot %d: \"%s\" is back - re-adopting their car",
                        i, g_rp[i].ownerName);
                } else {
                    // Somebody else now owns this slot. The wreck stays a wreck.
                    Log("[mp] slot %d taken by a new player; the previous car stays "
                        "parked for this race", i);
                    g_rp[i] = RemotePlayer{};
                    g_rp[i].sync.delayScale = g_cfg.interpDelay;
                }
            }
            char who[64], body[160];
            OthersShort(who, sizeof(who), LobbySize() - 1);
            if (here) sprintf_s(body, "%s joined (you + %s total)", nameBuf, who);
            else      sprintf_s(body, "%s left (you + %s total)", nameBuf, who);
            // Before the roster has settled, "here" only means "we have now heard
            // of them", which is not the same as them arriving. Log it, do not
            // toast it - that burst is the thing being fixed.
            if (rosterPrimed || !here) Notify(body);
            Log("[mp] %s", body);
        }
    }

    // Announce each player as they become ready, once per race.
    if (g_raceId != 0) {
        // Collect first, announce once. Two players reporting ready in the same
        // frame used to produce two toasts, and the engine queues toasts rather
        // than replacing them - so they arrive seconds apart.
        const char* justReady = nullptr;
        int newlyReady = 0;
        for (int i = 0; i < net::kMaxPeers; ++i) {
            net::Peer* p = net::GetPeer(i);
            if (!p || !net::PeerAlive(i) || i == MyId()) continue;
            const bool rdy = (p->flags & net::PF_READY) != 0;
            if (rdy && !g_rp[i].announcedReady) {
                g_rp[i].announcedReady = true;
                justReady = p->name; ++newlyReady;
            } else if (!rdy) {
                g_rp[i].announcedReady = false;
            }
        }
        if (newlyReady > 0) {
            char body[160];
            if (newlyReady == 1)
                sprintf_s(body, "%s is ready (%d of %d)", justReady, LobbyReady(), LobbySize());
            else
                sprintf_s(body, "%d more players ready (%d of %d)",
                          newlyReady, LobbyReady(), LobbySize());
            Notify(body);
            Log("[mp] %s", body);
        }
    }

    if (g_raceLive && g_holdCountdown && g_cfg.doCountdown) HoldCountdown();
    UpdateSelfReady();
    UpdateLabels();

    if (g_cfg.heartbeat && g_raceLive) {
        static float hbAcc = 0.f; static int hbN = 0;
        hbAcc += dt;
        if (hbAcc >= 1.0f) {
            hbAcc = 0.f;
            Log("[hb] %ds  cdHeld=%d preGame=%d startGate=%d phase=%d players=%d ready=%d",
                ++hbN, (int)CountdownHeld(), (int)PreGameHeld(), (int)StartGateHeld(),
                (int)g_phase, LobbySize(), LobbyReady());
        }
    }

    // Keep repeating GO until the retry budget runs out.
    if (g_goRepeats > 0) {
        g_goAcc += dt;
        if (g_goAcc > 0.25f) { g_goAcc = 0.f; --g_goRepeats; net::SendGo(g_goRaceId, g_myEpoch, g_goPhase); }
    }

    // A phase-1 GO that landed before our game object existed. Phase 1 needs a
    // LIVE race: with holdstart=load the gate can be held while CurrentGame()
    // still points at the front end or the race we just left.
    if (g_goPending && g_goPendingPhase == 1 && LoadGateHeld() && !g_raceLive) {
        ReleaseLoadGate(); g_phase = HELD;
        Log("[mp] deferred GO(1) released the load gate; the grid hold follows");
    }
    if (g_goPending && CurrentGame() &&
        (g_goPendingPhase == 0 ? (g_raceLive || LoadGateHeld()) : g_raceLive)) {
        const uint8_t ph = g_goPendingPhase;
        g_goPending = false;
        if (ph == 0) { ReleaseLoadGate(); g_phase = HELD; }
        else {
            g_goAppliedRace = g_raceId; g_goAppliedEpoch = g_ranEpoch;
            ReleaseLoadGate();
            if (PreGameHeld() || CountdownHeld()) {
                g_holdCountdown = false;
                ReleasePreGame(); ReleaseStartGate();
                if (g_cfg.doCountdown) StartCountdown();
                Notify("Get ready!");
            } else if (FireStartGate()) Notify("Go!");
        }
        Log("[mp] deferred GO(phase %u) applied", ph);
    }

    // ---------- client: obey the host ----------
    if (!net::IsHost()) {
        net::RaceBody rb{};
        // Phase first: TakeRaceCommand CONSUMES, so testing it first would destroy
        // a call that arrived a moment too early.
        // Not while the previous race is still unwinding. PreEnd sets g_phase to
        // IDLE immediately, so without this the consumer can fire on the very next
        // tick - inside the teardown, before the game has reached the menu.
        if (g_phase == IDLE && g_endAcc < 1.5f) {
            static double lastMoan = 0.0;
            if (GetTickCount64() - lastMoan > 2000.0) {
                lastMoan = (double)GetTickCount64();
                Log("[mp] holding a race command for %.1fs - the last race is still "
                    "unwinding", 1.5f - g_endAcc);
            }
        } else if (g_phase == IDLE && net::TakeRaceCommand(rb)) {
            // The host resends RACE while it is loading, so one of those resends
            // is normally still latched when our race ends and we drop to IDLE.
            // Reject only a resend of the exact race we already ran, from the same
            // host process - a restarted host numbers from 1 again but carries a
            // different epoch.
            if (g_ranRaceId != 0 && rb.raceId == g_ranRaceId && rb.hostEpoch == g_ranEpoch) {
                Log("[mp] ignoring a stale resend of race %u (already ran it)", rb.raceId);
                return;
            }
            g_ranEpoch = rb.hostEpoch;
            g_cfg.seed = rb.seed;
            g_raceId = rb.raceId;
            // The caller's rules, not ours. Our own [net] values stay in the ini
            // as the defaults we would use if WE called the race.
            AdoptRules(rb.rules, true,
                       (net::ServerFlags() & net::SF_HEADLESS) ? "server rules"
                                                               : "host's rules");
            RaceSpec spec{};
            strncpy_s(spec.track, rb.track, sizeof(spec.track)-1);
            strncpy_s(spec.setting, rb.setting, sizeof(spec.setting)-1);
            spec.gameType = rb.gameType; spec.lapCount = rb.lapCount; spec.mirror = rb.mirror != 0;
            spec.vehicleCount = rb.vehicleCount;
            g_askedFor = spec;
            SetBubbleSeed(g_cfg.seed); SeedRng(g_cfg.seed);
            net::ResetRaceState();
            g_raceHoldMode = g_cfg.holdMode;
            if (g_raceHoldMode & 4) { g_holdCountdown = true; if (g_cfg.doCountdown) HoldCountdown(); }
            if (g_raceHoldMode & 6) HoldStartGate();
            g_weOwnRace = true;
            if (StartRace(spec, (g_raceHoldMode & 1) != 0)) {
                g_phase = LOADING; g_phaseAcc = 0.f;
                Notify((net::ServerFlags() & net::SF_HEADLESS)
                       ? "Loading the server's race..." : "Loading the host's race...");
            } else {
                Log("[mp] StartRace failed - dropping this race");
                g_holdCountdown = false; ReleaseCountdown();
                ReleaseStartGate(); ReleaseLoadGate();
                g_weOwnRace = false;
                g_raceId = 0;
            }
        }

        // Tell the host we are parked and waiting, and keep saying so until it
        // answers. This has to cover EVERY hold, not just the loading screen:
        // with holdstart = grid or pregame there is no load gate at all.
        const bool weAreHolding = LoadGateHeld() ||
                                  (g_raceLive && (StartGateHeld() || PreGameHeld() || CountdownHeld()));
        if (weAreHolding && g_phaseAcc > 0.5f) {
            g_phaseAcc = 0.f;
            net::SendReady(g_raceId);
        }

        net::GoBody gb{};
        if (net::PeekGoCommand(gb)) {
            if (gb.phase == 1 && g_goAppliedRace != 0 &&
                g_goAppliedRace == gb.raceId && g_goAppliedEpoch == gb.hostEpoch) {
                net::DropGoCommand();          // already dropped the lights for this race
            } else if (gb.raceId != g_raceId ||
                       (g_ranEpoch != 0 && gb.hostEpoch != g_ranEpoch)) {
                // Not for the race we are in. If it is one we already ran, bin it;
                // otherwise it is for a race we have not been told about yet -
                // LEAVE IT LATCHED and act on it when we get there.
                if (g_ranRaceId != 0 && gb.raceId == g_ranRaceId && gb.hostEpoch == g_ranEpoch)
                    net::DropGoCommand();
            } else if (gb.phase == 0) {
                net::DropGoCommand();
                if (LoadGateHeld()) {
                    ReleaseLoadGate(); g_phase = HELD;
                    Log("[mp] load gate released by host");
                }
            } else {
                net::DropGoCommand();
                Log("[mp] host dropped the lights");
                if (!g_raceLive) {
                    if (!g_goPending) Log("[mp] GO arrived before the race was up - deferred");
                    g_goPending = true; g_goPendingPhase = 1;
                } else {
                    g_goAppliedRace = gb.raceId; g_goAppliedEpoch = gb.hostEpoch;
                    ReleaseLoadGate();
                    if (PreGameHeld() || CountdownHeld()) {
                        g_holdCountdown = false;
                        ReleasePreGame(); ReleaseStartGate();
                        if (g_cfg.doCountdown) StartCountdown();
                        Notify("Get ready!");
                    } else if (FireStartGate()) Notify("Go!");
                }
            }
        }
    }

    // ---------- host: drive the lobby ----------
    if (net::IsHost()) {
        // Keep the lobby view fresh on every client - this is what drives the
        // "[loading...]" labels and the ready counters everywhere.
        static float lobbyAcc = 0.f;
        lobbyAcc += dt;
        if (lobbyAcc > 0.5f) { lobbyAcc = 0.f; net::SendLobby(g_raceId); }

        if (g_cfg.autostart && !g_autoArmed && g_phase == IDLE && !g_raceLive &&
            !CurrentGame() && LobbySize() > 1 && g_cfg.track[0]) {
            g_autoArmed = true;
            BeginRace("autostart");
        }
        // Resend the call until our own race is live. A single lost RACE datagram
        // otherwise means a player never hears about the race at all.
        if ((g_phase == LOADING || g_phase == HELD) && !g_raceLive &&
            g_raceId != 0 && net::ConnectedCount() > 0) {
            if (g_phaseAcc > 0.5f) {
                g_phaseAcc = 0.f;
                net::SendRace(BuildRaceBody(g_raceId));
            }
        }
        if (g_phase == LOADING) {
            // Everyone parked, or nobody left to wait for.
            const int others = net::ConnectedCount();
            if (LoadGateHeld() && (others == 0 || LobbyReady() >= LobbySize())) {
                SendGoReliably(0);
                ReleaseLoadGate();
                g_phase = HELD;
                Log("[mp] everyone is ready - releasing the load gate");
            }
        }
    }

    // Re-issue the waiting message while we are actually still waiting.
    //
    // This used to fire every 4 seconds into a queue whose toasts live longer
    // than that, and Notify's identical-body guard was 3 seconds - so the repeat
    // always cleared the guard and always queued another. Leave a lobby sitting
    // at the gate for a minute and you have a quarter of an hour of toasts to
    // watch. Toasts CAN be dismissed now, but a nag that repeats every four
    // seconds is still noise whether or not it queues.
    //
    // Now it speaks when it has something NEW to say, and otherwise keeps quiet
    // apart from a slow heartbeat.
    if (g_cfg.popups) {
        const bool waitLoad = LoadGateHeld();
        const bool waitGrid = g_raceLive && (StartGateHeld() || PreGameHeld() || CountdownHeld());
        static float nag = 0.f;
        static char  lastNag[96] = "";
        if (!waitLoad && !waitGrid) { nag = 0.f; lastNag[0] = 0; }
        else {
            nag += dt;
            char body[96];
            const int total = LobbySize(), ready = LobbyReady();
            // WeStartRaces(), not IsHost(): an admin on a dedicated server is the
            // one holding the lights, and must not be told to wait for a host.
            if (!WeStartRaces())     sprintf_s(body, "Waiting for %s to start the race", StarterName());
            else if (total <= 1)     sprintf_s(body, "Nobody else connected - F10 starts anyway");
            else if (ready < total)  sprintf_s(body, "Waiting for %d of %d to load...", total - ready, total);
            else                     sprintf_s(body, "Everyone ready (%d) - press F10", total);

            const bool changed = strcmp(body, lastNag) != 0;
            // Something new to say: say it, but never more than one toast every
            // 5s, in case the ready count flaps. Nothing new: a 20s heartbeat,
            // comfortably longer than a toast lives.
            if ((changed && nag > 5.0f) || nag > 20.0f) {
                nag = 0.f;
                strncpy_s(lastNag, body, sizeof(lastNag) - 1);
                Notify(body);
            }
        }
    }

    // ---------- in-race: drive every remote car ----------
    if (!g_raceLive || !HaveId()) return;
    const int me = MyId();
    for (int i = 0; i < net::kMaxPeers; ++i) {
        if (i == me) continue;
        RemotePlayer& rp = g_rp[i];
        net::Peer* p = net::GetPeer(i);
        if (!rp.inThisRace || !rp.cfg || !p || !net::PeerAlive(i)) continue;
        if (rp.gone) {
            // A wreck should not keep somebody's freeze on it for the rest of the
            // race. Strip whatever we mirrored, once.
            // Look the entity up from cfg rather than trusting rp.ent: the leave
            // handler runs EARLIER in this same OnTick and nulls it, so this
            // branch never once fired and the comment above described an ordering
            // that does not happen. Harmless for a five-second freeze, not
            // harmless for a permanent effect on a car parked for the whole race.
            if (g_rules.syncEffects && !rp.effectsStripped && rp.cfg) {
                VehicleEntity wreck = rp.ent ? rp.ent : EntityForConfigVehicle(rp.cfg);
                if (wreck && wreck.isLive()) {
                    rp.effectsStripped = true;
                    ActiveEffect on[24];
                    const int nOn = ReadVehicleEffects(wreck, on, 24);
                    for (int a = 0; a < nOn; ++a)
                        StopVehicleEffect(wreck, on[a].nameHash, true);
                }
            }
            continue;                                // an abandoned car, left parked
        }
        // The slot may have been handed to somebody new mid-race. Their packets
        // must not drive the previous occupant's car.
        if (rp.ownerHash && rp.ownerHash != NameHash(p->name)) continue;

        rp.ent = EntityForConfigVehicle(rp.cfg);
        if (!rp.ent || !rp.ent.isLive()) continue;
        if (!HasCustomInput(rp.ent)) {
            InstallCustomInput(rp.ent, RemoteDrive, (void*)(intptr_t)i);
            continue;
        }
        if (!p->haveState) continue;

        // A player can stop sending without leaving: quit to the menu, alt-tab, a
        // stall. They stay PeerAlive because HELLO keeps ticking, but their last
        // STATE packet never expires - so the car used to sit pinned mid-corner
        // with the throttle open. Track how long since a NEW one arrived.
        if (p->lastTick != rp.appliedTick) rp.stateAge = 0.f;
        else                               rp.stateAge += dt;
        const bool stale = rp.stateAge > 2.0f;
        if (stale) {
            // Let it coast, and stop pose-writing so physics can settle it. Not a
            // wreck: the moment their packets resume this picks straight back up.
            //
            // Effects are deliberately left alone rather than mirrored: their
            // last report is frozen in time, so re-applying from it would
            // resurrect effects forever. The local copies expire on their own,
            // and the diff resumes the moment real packets do.
            if (rp.ent) ApplyInputs(rp.ent, VehicleState{});
            // And the claim edge detector has to forget what it saw. `ghost` is
            // "what was on their car LAST frame"; after a three-second stall it is
            // three seconds old, and diffing against it the moment packets resume
            // reports every effect the engine applied in between as a fresh hit -
            // a burst of claims for shots nobody fired.
            rp.ghostInit = false; rp.ghostCount = 0;
            continue;
        }

        // Feed the interpolator ONLY on a genuinely new snapshot. Pushing the same
        // packet every frame freezes the target between arrivals and makes the car
        // judder at speed.
        if (p->lastTick != rp.appliedTick) {
            rp.appliedTick = p->lastTick;
            VehicleState st{};
            memcpy(st.pos,  p->state.pos,  sizeof(st.pos));   memcpy(st.quat, p->state.quat, sizeof(st.quat));
            memcpy(st.lin,  p->state.lin,  sizeof(st.lin));   memcpy(st.ang,  p->state.ang,  sizeof(st.ang));
            st.steer = p->state.steer; st.throttle = p->state.throttle;
            st.buttons = p->state.buttons; st.flags = p->state.flags;
            st.timeUs  = p->state.timeUs;
            if (rp.sync.snaps == 0) st.flags |= 1u;     // first one is a placement
            PushRemoteState(rp.sync, st);
        }
        // ---- make their ghost HOLD what they are actually holding ----
        //
        // Their car here is pose-driven, but it still has a real power-up
        // controller, and ApplyInputs replays their fire button onto it. So
        // whatever this copy holds is what THIS machine shows them throwing.
        // Nothing in the pickup path agrees between machines - the bubble type is
        // a per-machine RNG draw, collection is a local PhysX trigger, and an
        // interpolated ghost sweeps a different set of boxes than the real car
        // did - so the only way the two agree is to be told.
        //
        // Diffed, not written blindly: only a real change reaches the engine.
        if (g_rules.syncPowerUps && rp.ent && rp.ent.isLive()) {
            const net::StateBody& sb = p->state;
            // DIFFED AGAINST THE CAR, not against what we last wrote.
            //
            // Remembering our own writes looked cheaper and was wrong: their ghost
            // is a real PhysX actor and trips pickup bubbles here, so the engine
            // fills its slots behind our back. Diffing against a remembered write
            // then matches, skips, and leaves the ghost holding a card its owner
            // never had - which is exactly the "you fired a rocket, I saw a
            // freeze" symptom this whole change exists to remove. It also means a
            // stall, a vehicle reset or a LoserHelper car's own random draw is
            // corrected on the next packet instead of never.
            PowerUpSlot have[2];
            ReadPowerUpSlots(rp.ent, have);
            for (int k = 0; k < 2; ++k) {
                const uint32_t h = sb.slots[k].nameHash;
                // They cannot name what they hold - so we cannot reproduce it.
                // Leave the slot exactly as it is: an out-of-date card beats a car
                // that cannot fire at all.
                if (h == net::kSlotUnknown) continue;

                const void* pu = h ? catalog::PowerUpFromHash(h) : nullptr;
                if (h && !pu) {
                    static int moaned = 0;
                    if (moaned++ < 5)
                        Log("[pu] slot %d of %d holds power-up %08x, which we cannot "
                            "resolve - leaving their card alone", k, i, h);
                    continue;
                }
                const int wantLevel = sb.slots[k].level;
                const int wantCount = sb.slots[k].count;
                if (have[k].powerUp == pu && have[k].level == wantLevel &&
                    have[k].count == wantCount) continue;
                ForcePowerUpSlot(rp.ent, k, pu, wantLevel, wantCount);
            }
        }

        // ---- mirror their power-up effects onto their car ----
        // Diffed against what is ACTUALLY on the car, not against a remembered
        // list of what we last applied. That matters more than it sounds:
        //
        //  * an apply the engine REFUSED (shield up, ReapplyType said ignore,
        //    car mid-respawn) is simply still missing next frame, so it retries
        //    instead of being recorded as done and never applied again;
        //  * an effect THIS machine applied locally - which is exactly what the
        //    shooter does today - is on the car and not in their report, so it
        //    gets removed. That is the phantom self-correcting, and it is the
        //    whole point of the victim owning their own status;
        //  * a doubled effect gets one instance removed per frame until it
        //    matches, rather than being stranded because we only remembered one.
        //
        // It is also stateless, so nothing has to be reset when a player leaves,
        // returns, or has their slot handed to somebody else.
        if (g_rules.syncEffects) {
            const net::StateBody& ps = p->state;
            const int want = (ps.effectCount <= net::kMaxWireEffects)
                           ? ps.effectCount : net::kMaxWireEffects;
            ActiveEffect on[24];
            const int nOn = ReadVehicleEffects(rp.ent, on, 24);

            // --- shooter authority: tell them what we just landed on them ---
            //
            // We do not own the projectile, so there is no hit callback to hook.
            // What we DO have is the engine applying the effect to our local copy
            // of their car - because both machines fire, our copy of their car is
            // where our own shot lands. An effect appearing on that copy which
            // they are not reporting IS our hit, and it is the only notification
            // of it we are ever going to get.
            //
            // That signal is not specific, though, and this is where the sharp
            // edges are. Three gates stand between "an effect appeared" and "we
            // shot them", and every one of them exists because of a way this goes
            // permanently wrong without it:
            //
            //  * EDGE, not level. `ghost` is last frame's set, so a claim fires on
            //    the transition only. Testing "is it on the car" would re-claim
            //    every frame, and since applyEffect always restarts the full
            //    duration, that is an unbreakable freeze rather than a small bug.
            //  * WE MUST HAVE FIRED. The engine also puts effects on cars for
            //    collisions, hazards, water and pickups. Without this gate a car
            //    that stalls in a hazard gets claimed, re-frozen, held in the
            //    hazard by the freeze, and claimed again - for the rest of the
            //    race, with nobody having pressed anything.
            //  * COOLDOWN. A backstop for whatever the first two miss. The worst
            //    it costs is one un-replicated follow-up hit; the worst it
            //    prevents is a car that can never be un-frozen.
            //
            // What it deliberately does NOT gate on is whether the shot was
            // "fair" - that is the victim's call, and the victim has the evidence
            // for it (see DrainHits). Claiming is cheap; accepting is where the
            // judgement lives.
            for (int k = 0; k < rp.coolCount; ) {
                rp.cool[k].left -= dt;
                if (rp.cool[k].left <= 0.f) rp.cool[k] = rp.cool[--rp.coolCount];
                else ++k;
            }
            if (g_rules.hitClaims && g_raceLive && !rp.gone && HaveId() && g_raceId != 0) {
                const VehicleEntity mine = LocalPlayerEntity();
                for (int a = 0; a < nOn && rp.ghostInit; ++a) {
                    const uint32_t h = on[a].nameHash;

                    // WAS IT US? Not inferred any more - the effect records the
                    // vehicle that caused it, and the engine uses that same field
                    // for its own "X hit you with Y" toast, so the meaning is not
                    // in doubt. Two whole classes of false claim die here:
                    //
                    //  * environmental. Water, lava, bog, a recover, a boost start
                    //    are all applied with a NULL originator, so they can no
                    //    longer read as "our shot landed" and re-freeze somebody
                    //    who simply drove into a puddle.
                    //  * bystanders. Every machine replays every player's fire
                    //    button, so with three or more people a third party's copy
                    //    of YOUR projectile can connect on their screen and they
                    //    would claim it under their own name. Now their copy is
                    //    stamped with your car, not theirs, and they stay quiet.
                    //
                    // The originator is a raw pointer with no generation counter,
                    // so it is compared and discarded in the same breath and never
                    // stored.
                    if (!mine || on[a].originator.p != mine.p) continue;
                    // ...and it must be on somebody ELSE'S car. With gridslots
                    // off, or with our id past the grid size, TakeMySlot leaves
                    // the human in config slot 0 and g_rp[0] is then our own car -
                    // where every self-buff we ever use has originator == us and a
                    // non-NULL power-up. Without this we would claim to have
                    // shielded the host every time we shielded ourselves.
                    if (rp.ent.p == mine.p) continue;
                    // And a power-up rather than a shunt. Ramming somebody also
                    // stamps our car on the collision effect, which is true but is
                    // not a hit - collisions are already one-sided here, and
                    // claiming them would replicate an event the other machine
                    // simulated differently.
                    if (!on[a].fromPowerUp) continue;

                    bool wasThere = false;
                    for (int k = 0; k < rp.ghostCount; ++k)
                        if (rp.ghost[k] == h) { wasThere = true; break; }
                    if (wasThere) continue;             // not new - nothing landed
                    // Reported means it is THEIRS: either their own machine put it
                    // on, or the mirror below did on their say-so. Claiming it back
                    // would be an echo, and the echo never stops.
                    bool reported = false;
                    for (int w = 0; w < want; ++w)
                        if (ps.effects[w].nameHash == h) { reported = true; break; }
                    if (reported) continue;
                    bool cooling = false;
                    for (int k = 0; k < rp.coolCount; ++k)
                        if (rp.cool[k].hash == h) { cooling = true; break; }
                    if (cooling) continue;

                    // The victim-clock timestamp of the pose we were actually
                    // shooting at. rp.sync.clock IS that: the interpolator plays
                    // their car back on their own timeline, so this is the exact
                    // instant of the picture we aimed at, in their units, with no
                    // estimate anywhere in it. Sending it is what lets them judge
                    // how stale our shot was without either machine having to
                    // guess at a clock offset or a latency.
                    if (rp.sync.snaps == 0) continue;      // no view of them to cite
                    // Never cite a pose we invented. Past the newest snapshot the
                    // interpolator dead-reckons on velocity for up to extrapMax
                    // (0.2 s), and during a dropout that is exactly where the aim
                    // error lives - the shooter is shooting at a straight-line
                    // guess while the victim is braking into a hairpin. Citing the
                    // guess would hand that error to the victim as zero drift;
                    // citing the last REAL snapshot makes a long dropout show up as
                    // the large drift it is, and be refused.
                    float viewT = rp.sync.clock;
                    if (viewT > rp.sync.t1) viewT = rp.sync.t1;
                    // Through int32_t: the interpolator slides its origin back by
                    // 600 s once a sender's clock passes ten minutes, which leaves
                    // clock momentarily negative, and float->unsigned on a negative
                    // value is undefined.
                    const uint32_t viewUs =
                        rp.sync.baseUs + (uint32_t)(int32_t)(viewT * 1e6f);

                    // Hard cap per (victim, effect) per race - the backstop that
                    // makes a runaway claim loop terminate rather than merely slow
                    // down.
                    int ts = -1;
                    for (int k = 0; k < rp.tallyCount; ++k)
                        if (rp.tally[k].hash == h) { ts = k; break; }
                    if (ts < 0) {
                        // FAILS CLOSED when the table is full. Falling through
                        // instead - which is what it used to do - meant the
                        // thirteenth distinct effect claimed against one player had
                        // no cap on it at all, and the cap is the only backstop that
                        // makes a runaway loop actually terminate rather than merely
                        // slow to one claim every couple of seconds.
                        if (rp.tallyCount >= (int)(sizeof(rp.tally)/sizeof(rp.tally[0]))) {
                            // Per PLAYER. It was a function-local static inside a
                            // loop over peers, so it fired once per process for
                            // whichever slot got there first and never again.
                            if (!rp.tallyFull) {
                                rp.tallyFull = true;
                                Log("[fx] slot %d has had claims for %d different "
                                    "effects this race - that is not a race, so no "
                                    "more are being sent to them", i, rp.tallyCount);
                            }
                            continue;
                        }
                        ts = rp.tallyCount++;
                        rp.tally[ts].hash = h; rp.tally[ts].sent = 0; rp.tally[ts].moaned = false;
                    }
                    if (rp.tally[ts].sent >= 12) {
                        if (!rp.tally[ts].moaned) {
                            rp.tally[ts].moaned = true;
                            Log("[fx] STOPPING claims of %s against slot %d for this "
                                "race - 12 already sent, which is not a game, it is a "
                                "loop", catalog::EffectName(h), i);
                        }
                        continue;
                    }
                    ++rp.tally[ts].sent;

                    const uint32_t evId = ++g_hitEventId;
                    const int lv = (on[a].level >= 0 && on[a].level <= 12) ? on[a].level : 0;
                    net::SendHit((uint8_t)i, h, evId,
                                 (uint32_t)(g_raceTime * 1e6), g_raceId, viewUs,
                                 (uint8_t)lv);

                    if (rp.coolCount < 8) rp.cool[rp.coolCount++] = { h, g_rules.hitCooldown };

                    // Hold the effect on our copy while they answer, instead of
                    // letting the ordinary grace period strip it out from under a
                    // claim still in flight.
                    int cs = -1;
                    for (int k = 0; k < rp.pendingCount; ++k)
                        if (rp.pending[k].hash == h) { cs = k; break; }
                    if (cs < 0 && rp.pendingCount < 8) {
                        cs = rp.pendingCount++;
                        rp.pending[cs].hash = h; rp.pending[cs].age = 0.f;
                    }
                    if (cs >= 0) { rp.pending[cs].claimed = true; rp.pending[cs].age = 0.f; }

                    Log("[fx] we hit slot %d (\"%s\") with %s - claim #%u sent",
                        i, rp.ownerName, catalog::EffectName(h), evId);
                }
            }
            // --- take off what they do not report, but not straight away ---
            //
            // BOTH machines fire. The buttons are in every state packet, so when
            // you pull the trigger their machine sees your car pull it too and
            // runs its own copy of the projectile. Usually both land and both
            // agree - which is why hits mostly worked before any of this existed.
            //
            // But they do not land at the same INSTANT: ours lands now, theirs
            // lands a moment later, and their confirmation takes another hop to
            // reach us. Removing an unreported effect immediately would take the
            // freeze off, then put it back when their packet arrives - a visible
            // flicker on every ordinary hit. So an unreported effect has to stay
            // unreported for a grace period before we believe it.
            bool stripped = false, touched = false;
            // Retire timers for effects that are no longer on the car at all,
            // BEFORE the loop below rather than after. Doing it first is what
            // keeps the eight slots from being clogged by entries whose effect
            // ended on its own - which used to leave a genuine phantom with no
            // timer and therefore no way to ever come off.
            for (int k = 0; k < rp.pendingCount; ) {
                bool live = false;
                for (int a = 0; a < nOn; ++a)
                    if (on[a].nameHash == rp.pending[k].hash) { live = true; break; }
                if (live) ++k; else rp.pending[k] = rp.pending[--rp.pendingCount];
            }
            // Age every timer exactly ONCE per frame. It used to be aged inside
            // the loop below, which runs per effect ON THE CAR - so a doubled
            // effect (two instances of the same hash, which this code explicitly
            // expects) aged its timer at double rate and got stripped after half
            // the grace period, reintroducing the flicker the grace exists for.
            for (int k = 0; k < rp.pendingCount; ++k) rp.pending[k].age += dt;

            for (int a = 0; a < nOn; ++a) {
                bool reported = false;
                for (int w = 0; w < want; ++w)
                    if (ps.effects[w].nameHash == on[a].nameHash) { reported = true; break; }

                int slot = -1;
                for (int k = 0; k < rp.pendingCount; ++k)
                    if (rp.pending[k].hash == on[a].nameHash) { slot = k; break; }

                if (reported) {                       // confirmed - forget the timer
                    if (slot >= 0) rp.pending[slot] = rp.pending[--rp.pendingCount];
                    continue;
                }
                if (slot < 0) {
                    // Eight unreported effects at once on one car does not happen;
                    // if it somehow does, the untracked ones simply stay on until a
                    // slot frees, which is the harmless direction.
                    if (rp.pendingCount >= 8) continue;
                    slot = rp.pendingCount++;
                    rp.pending[slot].hash = on[a].nameHash;
                    rp.pending[slot].age  = 0.f;       // aged from the NEXT frame
                    rp.pending[slot].claimed = false;
                    continue;
                }
                // A claim we sent gets the longer window: it has to survive the
                // retransmits, the round trip, their apply and their next state
                // packet before silence is evidence of anything.
                const bool wasClaim = rp.pending[slot].claimed;
                const float limit = wasClaim ? g_rules.hitTimeout : g_rules.effectGrace;
                if (rp.pending[slot].age < limit) continue;
                if (wasClaim)
                    Log("[fx] slot %d (\"%s\") never confirmed our %s - taking it "
                        "back off (refused as implausible, or every copy of the "
                        "claim was lost)",
                        i, rp.ownerName, catalog::EffectName(on[a].nameHash));
                // Silent: a correction, not the effect ending, so no ice-shatter
                // for something that was never really there. Every instance goes,
                // not one per frame: an unreported effect is unreported however
                // many copies of it are on the car, and taking them off one 0.6 s
                // grace period at a time left a doubled phantom visible for
                // seconds.
                int dupes = 0;
                for (int k = 0; k < nOn; ++k) if (on[k].nameHash == on[a].nameHash) ++dupes;
                for (int k = 0; k < dupes; ++k)
                    StopVehicleEffect(rp.ent, on[a].nameHash, true);
                stripped = true;
                rp.pending[slot] = rp.pending[--rp.pendingCount];
            }

            // --- put on what they report and we do not have ---
            for (int w = 0; w < want; ++w) {
                const uint32_t h = ps.effects[w].nameHash;
                bool present = false;
                for (int a = 0; a < nOn; ++a)
                    if (on[a].nameHash == h) { present = true; break; }
                if (present) continue;

                // Nearly over. applyEffect always starts a FRESH full duration -
                // we cannot ask for "2 tenths of a freeze" - so mirroring the
                // dying end of an effect would re-freeze the car for another five
                // seconds after theirs had finished. Let it go.
                const uint8_t tenths = ps.effects[w].tenths;
                if (tenths != 255 && tenths < 5) continue;

                const char* nm = catalog::EffectName(h);
                if (!nm[0]) {
                    static int moaned = 0;
                    if (moaned++ < 5)
                        Log("[fx] slot %d reports effect %08x, which is not in "
                            "VehicleEffectDB - ignoring", i, h);
                    continue;
                }
                // Backed off only after a REFUSAL. The throttle used to run on
                // every apply and `break` out of the loop, which quietly serialised
                // distinct effects: a car caught by two power-ups in the same frame
                // showed the second one a quarter of a second late on every other
                // machine. What actually needs throttling is the retry - a shield
                // can hold for seconds, and asking 30 times a second is pointless.
                if (rp.applyAcc > 0.f) { rp.applyAcc -= dt; continue; }
                // No originator: we are mirroring what they told us is on their
                // car, and we do not know who put it there. Claiming OUR vehicle
                // here would be a lie the claim emitter would then act on - it
                // tests exactly this field - and the effect would be claimed
                // straight back at them, forever.
                if (!ApplyVehicleEffect(rp.ent, nm, 0)) rp.applyAcc = 0.25f;
                else                                    touched = true;
            }

            // Refresh the snapshot LAST - after the strip AND after the mirror-add
            // - and from what is really on the car.
            //
            // Three traps here, all found the hard way, all the same shape: `ghost`
            // must describe the car as it is at the END of this frame, or the next
            // frame's edge test lies.
            //
            //  * Inside the claim block: any skipped frame left it seconds old, and
            //    the next frame that ran claimed everything that had changed since.
            //  * Before the strip: an effect we had just removed was still recorded
            //    as present, so when the engine re-applied it the transition was
            //    invisible - no claim ever again, and a silent on/off flicker.
            //  * Before the mirror-add: an effect we had just applied ON THEIR OWN
            //    SAY-SO was missing from it, so if their very next packet dropped
            //    that effect (they respawned, took a cure, or it fell out of the
            //    six-effect wire budget) we read it as a fresh hit and claimed it
            //    straight back at them - re-freezing a player the instant they
            //    cleared it, which is exactly the echo the `reported` test exists
            //    to prevent.
            ActiveEffect fresh[24];
            const ActiveEffect* gsrc = on;
            int gn = nOn;
            if (stripped || touched) { gn = ReadVehicleEffects(rp.ent, fresh, 24); gsrc = fresh; }
            rp.ghostCount = 0;
            for (int a = 0; a < gn && rp.ghostCount < 24; ++a)
                rp.ghost[rp.ghostCount++] = gsrc[a].nameHash;
            rp.ghostInit = true;
        }

        // ...but drive the car EVERY frame off the continuous interpolated target.
        TickRemoteSync(rp.sync, rp.ent, dt);
    }

    static int fr = 0;
    if (++fr >= 300) {
        fr = 0;
        VehicleState mine{};
        if (CaptureState(LocalPlayerEntity(), mine)) {
            for (int i = 0; i < net::kMaxPeers; ++i) {
                if (i == me || !g_rp[i].ent) continue;
                float tp[3]{};
                if (!RemoteSyncTarget(g_rp[i].sync, tp)) continue;
                float dx=tp[0]-mine.pos[0], dy=tp[1]-mine.pos[1], dz=tp[2]-mine.pos[2];
                net::Peer* p = net::GetPeer(i);
                Log("[mp] slot %d \"%s\": gap=%.1fm packet=%.0fms buffer=%.0fms jitter=%.0fms",
                    i, p ? p->name : "?", sqrtf(dx*dx+dy*dy+dz*dz),
                    g_rp[i].sync.interval*1000.f, g_rp[i].sync.lead*1000.f,
                    g_rp[i].sync.jitter*1000.f);
            }
        }
    }
}

void ModMain() {
    LoadConfig();
    g_guiStartMs = GetTickCount64();
    // Identifies THIS host process; only has to differ across restarts.
    g_myEpoch = (uint32_t)GetTickCount() ^ ((uint32_t)GetCurrentProcessId() << 16) ^ 0x9E3779B9u;
    AdoptRules(RulesFromConfig(), false, "local rules until a race is called");
    UseSimplePrepare(g_cfg.simpleHud);
    SetToastStyle(g_cfg.toastStyle);
    SetToastDismiss(g_cfg.toastDrain);
    SetRaceConfigCallback(ApplyRoster, nullptr);
    if (g_cfg.vehicle[0]) TraceVehicleLoads(true);
    ResetPlayers();

    if (!net::Init((uint16_t)g_cfg.port, g_cfg.hostAddr, (uint16_t)g_cfg.hostPort,
                   g_cfg.host, g_cfg.maxPlayers))
        Log("[mp] FATAL: %s", net::LastError());
    else
        // The protocol version belongs in the LOG, not only in the F6 panel. The
        // server prints its version in its banner; the client did not print its
        // anywhere a log file could show it - so "which machine is out of date"
        // meant guessing, on exactly the machine that cannot connect to ask.
        Log("[mp] UDP %d, %s  |  protocol v%d", g_cfg.port,
            net::IsHost() ? "hosting" : "client (id assigned on join)",
            (int)net::kProtocolVersion);
    net::SetLocalIdentity(MyIdentity());
    net::LogLocalAddresses();

    for (int i = 0; i < 12; ++i)
        if (const char* n = GameTypeName(i)) Log("[mp] gameType %2d = %s", i, n);
    // Not fatal: without it every player sees different icons on the pickup
    // bubbles. Everything else still works, so say so loudly rather than dying.
    // Seed from the ini now rather than waiting for a race to be called. The
    // bubbles are enabled during level load, which happens BEFORE PreBegin - so a
    // race started from the game's own menus would otherwise get no seed at all.
    SetBubbleSeed(g_cfg.seed);
    if (!HookPowerUpBubbles())
        Log("[mp] WARNING: could not hook the pickup bubbles - their types will "
            "NOT match between machines");
    if (!HookGameBegin(PreBegin, PostBegin)) {
        // Was logged FATAL and then ignored, which is the worst of both. Without
        // the begin hook g_gameObj is never set, so EntityCount() is 0 forever and
        // the mod runs inert while looking alive: no roster, no remote cars, no
        // effects, and nothing to say why. end() already disables the mod on
        // failure; begin() is no less load-bearing.
        Log("[mp] FATAL: could not hook begin() - disabling the mod rather than "
            "running inert and pretending to work");
        return;
    }
    if (!HookGameEnd(PreEnd, nullptr)) {
        // Not survivable: PreEnd is the only thing that clears g_raceLive and the
        // cached pointers. Without it the first tick after a race ends
        // dereferences a freed game object.
        Log("[mp] FATAL: could not hook end() - disabling the mod rather than "
            "running into freed memory after the first race");
        SetTickCallback(nullptr, nullptr);
        SetCaptureCallback(nullptr, nullptr);
        return;
    }
    SetTickCallback(OnTick, nullptr);
    SetCaptureCallback(OnCapture, nullptr);
    Hello();
    atexit(OnExit);

    // The overlay goes up LAST. gui::Init creates a real D3D11 device to read the
    // swapchain vtable, and first use of the graphics driver can take a second or
    // more - long enough for the game to reach begin() with our hooks not yet in.
    //
    // First attempt. It will usually fail here - VuTuningManager::load() has not
    // run yet this early - and OnTick keeps asking until it takes. The lists are
    // immutable once catalog::Ready() flips, which is what makes them safe for
    // the render thread to read without copying anything.
    if (g_cfg.guiEnabled || g_cfg.syncEffects || g_cfg.syncPowerUps) catalog::Build();
    {
        gui::Options o;
        o.enabled   = g_cfg.guiEnabled;
        o.toggleKey = g_cfg.guiKey;
        o.scale     = g_cfg.guiScale;
        o.eatInput  = g_cfg.guiEatInput;
        if (o.toggleKey == 0) { o.enabled = false; Log("[gui] no toggle key set - overlay off"); }
        if (gui::Init(o)) gui::SetPanel(OverlayPanel);
    }
    if (gui::Installed())
        Log("[mp] HOTKEYS:  F6 = overlay (or whatever [gui] key says)");
    Log("[mp] HOTKEYS:  F9 = call the race (host)    F7 = lobby status");
    Log("[mp]           F10/F11 = GO                 F8 = full status dump");
    Log("[mp] Sit at the MAIN MENU. Clients need do nothing - the host calls the race.");
    Log("[mp] holdstart mode = %d (0 off, 1 loading screen, 2 grid, 3 both, 4 pregame)",
        g_cfg.holdMode);
}
