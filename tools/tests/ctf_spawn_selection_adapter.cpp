// Minimal types only: the selector, commit and restart method are inserted verbatim.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using int32 = int32_t;
using uint8 = uint8_t;
using uint32 = uint32_t;
using TCHAR = char;
#define TEXT(value) value
constexpr int32 INDEX_NONE = -1;
constexpr int ROLE_AutonomousProxy = 2;
constexpr int COLLISION_TRACE_WEAPONNOCHARACTER = 1;
struct FString : std::string {
    using std::string::string;
    const char* operator*() const { return c_str(); }
};
using FName = FString;
struct FVector {
    float X = 0.f, Y = 0.f, Z = 0.f;
    FVector() = default;
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
    FVector operator+(const FVector& rhs) const { return FVector(X+rhs.X, Y+rhs.Y, Z+rhs.Z); }
    FVector operator-(const FVector& rhs) const { return FVector(X-rhs.X, Y-rhs.Y, Z-rhs.Z); }
    FVector operator*(float amount) const { return FVector(X*amount, Y*amount, Z*amount); }
    FVector& operator+=(const FVector& rhs) { X+=rhs.X; Y+=rhs.Y; Z+=rhs.Z; return *this; }
    float SizeSquared() const { return X*X+Y*Y+Z*Z; }
    float Size() const { return std::sqrt(SizeSquared()); }
    static float DistSquared(const FVector& a, const FVector& b) { return (a-b).SizeSquared(); }
    FString ToString() const { char value[128]; std::snprintf(value, sizeof(value), "X=%.1f Y=%.1f Z=%.1f", X,Y,Z); return value; }
    static const FVector ZeroVector;
};
const FVector FVector::ZeroVector;
struct FMath {
    template<class T> static T Min(T a, T b) { return std::min(a,b); }
    template<class T> static T Max(T a, T b) { return std::max(a,b); }
    template<class T> static T Clamp(T a, T lo, T hi) { return Max(lo, Min(a,hi)); }
    template<class T> static T Square(T a) { return a*a; }
    static int32 TruncToInt(float value) { return static_cast<int32>(value); }
    static float FRandRange(float lo, float hi) { return (lo+hi)*.5f; }
    static int32 RandRange(int32 lo, int32) { return lo; }
};
struct UObject { bool PendingKill = false; virtual ~UObject() = default; };
template<class T> bool IsValid(T* value) { return value && !value->PendingKill; }
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
template<class T> struct TWeakObjectPtr {
    T* Value = nullptr;
    TWeakObjectPtr() = default;
    TWeakObjectPtr(T* value) : Value(value) {}
    T* Get() const { return ::IsValid(Value) ? Value : nullptr; }
    T* operator->() const { return Get(); }
    bool IsValid() const { return Get() != nullptr; }
    bool operator<(const TWeakObjectPtr& rhs) const { return std::less<T*>()(Value, rhs.Value); }
    bool operator==(const TWeakObjectPtr& rhs) const { return Value == rhs.Value; }
    TWeakObjectPtr& operator=(T* value) { Value=value; return *this; }
};
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    int32 Num() const { return static_cast<int32>(this->size()); }
    void Add(const T& value) { this->push_back(value); }
    void Reserve(int32 value) { this->reserve(static_cast<size_t>(value)); }
    void Reset() { this->clear(); }
    void Init(const T& value, int32 count) { this->assign(static_cast<size_t>(count),value); }
    void Empty() { this->clear(); }
    void RemoveAt(int32 index, int32 count, bool) { this->erase(this->begin()+index, this->begin()+index+count); }
    bool Contains(const T& value) const { return std::find(this->begin(), this->end(), value) != this->end(); }
    template<class Predicate> int32 IndexOfByPredicate(Predicate predicate) const {
        for (int32 i=0; i<Num(); ++i) if (predicate((*this)[i])) return i;
        return INDEX_NONE;
    }
};
template<class K, class V> struct TMap {
    std::map<K,V> Values;
    V& FindOrAdd(const K& key) { return Values[key]; }
    V* Find(const K& key) { auto it=Values.find(key); return it==Values.end() ? nullptr : &it->second; }
    const V* Find(const K& key) const { auto it=Values.find(key); return it==Values.end() ? nullptr : &it->second; }
    void Add(const K& key, const V& value) { Values[key]=value; }
    int32 Num() const { return static_cast<int32>(Values.size()); }
};
struct AActor : UObject {
    FString Name;
    FVector Location;
    FVector GetActorLocation() const { return Location; }
    FString GetName() const { return Name; }
};
FString GetNameSafe(const AActor* value) { return value ? value->GetName() : FString("None"); }
struct APlayerStart : AActor { float StockRating = 1.f; };
struct APawn : AActor {};
struct APlayerState : UObject {};
struct AUTTeamInfo : UObject { uint8 TeamIndex = 0; };
struct AUTPlayerState : APlayerState {
    FString PlayerName;
    AUTTeamInfo* Team = nullptr;
    AUTPlayerState* LastKillerPlayerState = nullptr;
    bool bOnlySpectator = false, bIsInactive = false;
    float ExactPing = 0.f;
};
struct AUTFlag : AActor { APawn* HoldingPawn = nullptr; };
struct AUTCharacter : APawn {
    float BaseEyeHeight = 0.f;
    int32 Health = 100;
    bool Dead = false;
    int32 RemoteRole = 0;
    FVector Velocity;
    AActor* CarriedObject = nullptr;
    bool IsPendingKillPending() const { return PendingKill; }
    bool IsDead() const { return Dead; }
    int32 GetRemoteRole() const { return RemoteRole; }
    FVector GetVelocity() const { return Velocity; }
    AActor* GetCarriedObject() const { return CarriedObject; }
    AUTCharacter* GetClass() { return this; }
    template<class T> T* GetDefaultObject() { return static_cast<T*>(this); }
};
struct AController : UObject {
    APlayerState* PlayerState = nullptr;
    APawn* Pawn = nullptr;
    TWeakObjectPtr<AActor> StartSpot;
    APawn* GetPawn() const { return Pawn; }
};
struct AUTCTFFlagBase : AActor { AUTFlag* MyFlag = nullptr; };
namespace CarriedObjectState { constexpr int Home = 0; constexpr int Held = 2; }
struct AUTCTFGameState : UObject {
    AUTCTFFlagBase* Bases[2] = {nullptr,nullptr};
    int States[2] = {0,0};
    int GetFlagState(uint8 team) const { return States[team]; }
    AUTCTFFlagBase* GetFlagBase(uint8 team) const { return Bases[team]; }
};
struct ControllerRef { AController* Value; AController* Get() const { return Value; } };
struct FConstControllerIterator {
    const std::vector<ControllerRef>* Values;
    size_t Index = 0;
    explicit operator bool() const { return Index < Values->size(); }
    FConstControllerIterator& operator++() { ++Index; return *this; }
    const ControllerRef* operator->() const { return &(*Values)[Index]; }
};
struct FCollisionQueryParams { FCollisionQueryParams(const FName&, bool) {} };
struct UWorld {
    std::vector<ControllerRef> Controllers;
    bool LineBlocked = true;
    std::function<bool(const FVector&, const FVector&)> TraceBlocked;
    float TimeSeconds = 1.f;
    FConstControllerIterator GetControllerIterator() { return {&Controllers,0}; }
    bool LineTraceTestByChannel(const FVector& from, const FVector& to, int, const FCollisionQueryParams&) const {
        return TraceBlocked ? TraceBlocked(from,to) : LineBlocked;
    }
    float GetTimeSeconds() const { return TimeSeconds; }
};
std::vector<std::string> Logs;
template<class... Args> void CaptureLog(const char* format, Args... args) {
    char line[2048]; std::snprintf(line,sizeof(line),format,args...); Logs.emplace_back(line);
}
#define UE_LOG(category,level,...) CaptureLog(__VA_ARGS__)
struct StockGameMode {
    bool SpawnSucceeds = true;
    APawn CreatedPawn;
    void RestartPlayerAtPlayerStart(AController* player, AActor*) {
        if (SpawnSucceeds && player && !player->Pawn) player->Pawn=&CreatedPawn;
    }
    float RatePlayerStart(APlayerStart* start, AController*) { return start->StockRating; }
    AActor* ChoosePlayerStart_Implementation(AController*) { return nullptr; }
};
struct FGameSession { int32 MaxPlayers=16; };
struct ANCPlusCTFGameMode : StockGameMode {
    using Super = StockGameMode;
    // PRODUCTION_HISTORY
    TMap<TWeakObjectPtr<AController>, FRecentSpawns> PlayerRecentSpawns;
    TMap<TWeakObjectPtr<APlayerStart>, float> SpawnLastUsedTime;
    TArray<TWeakObjectPtr<APlayerStart>> Team0Spawns, Team1Spawns;
    UWorld World;
    UWorld* GetWorld() { return &World; }
    AUTCTFGameState* CTFGameState = nullptr;
    FGameSession* GameSession = nullptr;
    bool bSpawnUseNewCTFSelection = true, bSpawnPoolsBuilt = true, bLogSpawnChoices = false;
    // Existing cases retain the pre-change iCTF route; weapon-CTF cases opt in below.
    bool bIsInstagib = true;
    bool bSpawnExtrapolateMovement = true, bSpawnSecondaryEnabled = true, Live = true;
    int32 SpawnMinCycleDistance = 1, RobbedSpawnRotation[2] = {0,0};
    float SpawnRobbedBaseAvoidCount = 0.f;
    float EnemyLOSBlockRange = 0.f, SpawnFlagCarrierLOSAvoidRadius = 0.f;
    float SpawnEnemyBelowZ = 190.f, SpawnEnemyHardRadius = 0.f;
    float SpawnFriendlyVisionBlockRange = 0.f, SpawnFriendlyBlockRange = 0.f;
    float SpawnFlagBlockRange = 0.f, SpawnKillerAvoidRadius = 0.f;
    float SpawnSecondaryMaxDistance = 2000.f, SpawnSecondaryOwnTeamWeight = .2f, SpawnSecondaryCarrierWeight = 2.f;
    float SpawnFreshnessBonus=0.f, SpawnFreshnessWindow=30.f, SpawnTieBandWidth=0.f;
    float SpawnRandomBase=1.f, SpawnRandomSpread=1.f;
    bool bSpawnWeightedRandom=false;
    int32 SpawnSystemThreshold = 4, Competitors = 10;
    int32 StockCalls = 0, LegacyCalls = 0;
    AActor* StockResult = nullptr;
    AActor* LegacyResult = nullptr;
    int32 CountSpawnSystemCompetitors() const { return Competitors; }
    AActor* ChooseEpicPlayerStart(AController*) { ++StockCalls; return StockResult; }
    AActor* ChooseLegacyPlayerStart(AController*) { ++LegacyCalls; return LegacyResult; }
    bool IsLiveSpawnCommitState() const { return Live; }
    bool IsFlagNearOwnBase(uint8) const { return false; }
    void BuildTeamSpawnPools() { bSpawnPoolsBuilt=true; }
    void RestartPlayerAtPlayerStart(AController*, AActor*);
    void CommitUsedSpawn(AController*, APlayerStart*);
    AActor* ChoosePlayerStart_Implementation(AController*);
    APlayerStart* ChooseNewCTFPlayerStart(AController*);
    AActor* ChooseLegacyPlayerStartNative(AController*);
};
// PRODUCTION_FUNCTIONS

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        for (const auto& line : Logs) std::cerr << line << '\n';
        std::exit(1);
    }
}
bool HasLog(const char* text) {
    for (const auto& line : Logs) if (line.find(text) != std::string::npos) return true;
    return false;
}
struct Fixture {
    ANCPlusCTFGameMode Game;
    AUTTeamInfo Team, EnemyTeam;
    AUTPlayerState VictimPS, TeammatePS, EnemyPS;
    AController Victim, Teammate, Enemy;
    AUTCharacter EnemyPawn;
    APlayerStart A, B, C, Duplicate;
    Fixture() {
        Logs.clear(); Team.TeamIndex=0; EnemyTeam.TeamIndex=1;
        VictimPS.Team=&Team; VictimPS.PlayerName="unbreaK"; Victim.PlayerState=&VictimPS;
        TeammatePS.Team=&Team; TeammatePS.PlayerName="d1smay"; Teammate.PlayerState=&TeammatePS;
        EnemyPS.Team=&EnemyTeam; EnemyPS.PlayerName="flow"; Enemy.PlayerState=&EnemyPS; Enemy.Pawn=&EnemyPawn;
        A.Name="A"; A.Location=FVector(0,0,0);
        B.Name="B"; B.Location=FVector(1000,0,0);
        C.Name="C"; C.Location=FVector(2000,0,0);
        Duplicate.Name="A_duplicate"; Duplicate.Location=FVector(40,30,0);
        SetPool({&A,&B,&C});
    }
    void SetPool(std::initializer_list<APlayerStart*> starts) {
        Game.Team0Spawns.clear(); for (auto* start : starts) Game.Team0Spawns.Add(start);
    }
    void Commit(AController& player, APlayerStart& start) { Game.CommitUsedSpawn(&player,&start); }
    void BlockPrimary() {
        Game.World.Controllers.push_back({&Enemy});
        Game.SpawnEnemyHardRadius=100000.f;
        Game.SpawnEnemyBelowZ=0.f;
        EnemyPawn.Location=FVector(2000,0,0);
    }
    APlayerStart* Choose() { return Game.ChooseNewCTFPlayerStart(&Victim); }
};
void Acrony() {
    Fixture f;
    f.Commit(f.Victim,f.A); f.Commit(f.Teammate,f.C); f.Commit(f.Teammate,f.B);
    // Queue A,C,B: teammate activity rotated the victim's previous start to the head.
    Require(f.Choose()==&f.C, "Acrony repeat survived teammate's intervening spawn");
    Require(!HasLog("spawn emergency"), "ordinary distinct choice emitted emergency");
    f.Commit(f.Victim,f.C); f.Commit(f.Teammate,f.B);
    Require(f.Choose()==&f.A, "older player history should not become a permanent blacklist");
}
void Secondary() {
    Fixture f; f.Game.bLogSpawnChoices=true;
    f.Commit(f.Victim,f.A); f.Commit(f.Teammate,f.B); f.BlockPrimary();
    Require(f.Choose()==&f.C, "secondary chose previous A, whose distance score is highest");
    Require(HasLog("system=secondary"), "test did not enter secondary pass");
    Require(!HasLog("reason=no-distinct-team-start"), "secondary wrongly relaxed last-start exclusion");
}
void Positions() {
    for (bool secondary : {false,true}) {
        Fixture f; f.Game.SpawnMinCycleDistance=0;
        f.Commit(f.Victim,f.A);
        f.Duplicate.Location=FVector(60,80,0); // Inclusive 100uu boundary.
        f.B.Location=FVector(0,0,101); // Same XY, distinct floor outside 3D radius.
        f.SetPool({&f.Duplicate,&f.B,&f.A});
        if (secondary) f.BlockPrimary();
        Require(f.Choose()==&f.B, "100uu duplicate or XY-only floor exclusion is wrong");
    }
}
void InvalidHistory() {
    Fixture f; f.Game.SpawnMinCycleDistance=0;
    f.Commit(f.Victim,f.A); f.A.PendingKill=true;
    f.SetPool({&f.A,&f.Duplicate,&f.B});
    Require(f.Choose()==&f.B, "destroyed old actor erased saved-location exclusion");
    Require(f.Game.Team0Spawns.Num()==2, "invalid weak pointer was not purged from team pool");
}
void MovedStart() {
    Fixture f; f.Game.SpawnMinCycleDistance=0;
    f.Commit(f.Victim,f.A); f.A.Location=FVector(-1000,0,0);
    f.SetPool({&f.A,&f.Duplicate,&f.B});
    Require(f.Choose()==&f.B, "moved old actor identity or original location was not excluded");
}
void First() {
    Fixture f;
    Require(f.Choose()==&f.A, "first spawn incorrectly excludes world origin");
    Require(f.Game.PlayerRecentSpawns.Num()==0, "preview created player history");
    Require(!HasLog("spawn emergency"), "first spawn wrongly treated as repeat emergency");
}
void CycleTail() {
    for (bool secondary : {false,true}) {
        Fixture f;
        f.Commit(f.Victim,f.A);
        f.SetPool({&f.A,&f.Duplicate,&f.B});
        if (secondary) f.BlockPrimary();
        Require(f.Choose()==&f.B, "team-cycle tail not relaxed before previous-spawn restriction");
        Require(HasLog("reason=team-cycle-exhausted"), "cycle relaxation must log with spawn logging disabled");
        Require(!HasLog("reason=no-distinct-team-start"), "distinct tail candidate treated as forced repeat");
    }
}
void Emergency() {
    for (bool instagib : {false,true}) for (bool colocated : {false,true}) for (bool secondary : {false,true}) {
        Fixture f; f.Game.bIsInstagib=instagib; f.Commit(f.Victim,f.A);
        if (colocated) f.SetPool({&f.Duplicate,&f.A}); else f.SetPool({&f.A});
        if (secondary) f.BlockPrimary();
        auto* chosen=f.Choose();
        Require(chosen!=nullptr, "all-repeat pool deadlocked instead of emergency spawning");
        Require(HasLog("reason=no-distinct-team-start"), "forced repeat was not unconditionally logged");
    }
}
void SecondaryOff() {
    Fixture f; f.Game.bSpawnSecondaryEnabled=false; f.Game.bLogSpawnChoices=true;
    f.Commit(f.Victim,f.A); f.Commit(f.Teammate,f.B); f.BlockPrimary();
    f.A.StockRating=100.f; f.C.StockRating=-10.f;
    Require(f.Choose()==&f.C, "secondary-disabled fallback bypassed previous-start exclusion");
    Require(HasLog("system=stock-rating"), "disabled secondary did not exercise filtered stock rating");
}
void CommitLifecycle() {
    Fixture f;
    const auto original=f.Game.Team0Spawns;
    Require(f.Choose()==&f.A && f.Choose()==&f.A, "preview unexpectedly advanced rotation");
    Require(f.Game.PlayerRecentSpawns.Num()==0 && f.Game.Team0Spawns==original,
        "preview modified successful-spawn history or queue");
    f.Game.SpawnSucceeds=false; f.Game.RestartPlayerAtPlayerStart(&f.Victim,&f.A);
    Require(f.Game.PlayerRecentSpawns.Num()==0 && f.Game.Team0Spawns==original,
        "failed spawn committed a start");
    APawn existing; f.Victim.Pawn=&existing; f.Game.SpawnSucceeds=true;
    f.Game.RestartPlayerAtPlayerStart(&f.Victim,&f.A);
    Require(f.Game.PlayerRecentSpawns.Num()==0, "existing pawn committed a new spawn");
    f.Victim.Pawn=nullptr; f.Game.Live=false;
    f.Game.RestartPlayerAtPlayerStart(&f.Victim,&f.A);
    Require(f.Game.PlayerRecentSpawns.Num()==0, "nonlive introduction spawn committed history");
    f.Victim.Pawn=nullptr; f.Game.Live=true;
    f.Game.RestartPlayerAtPlayerStart(&f.Victim,&f.A);
    Require(f.Game.PlayerRecentSpawns.Num()==1 && f.Game.Team0Spawns.back().Get()==&f.A,
        "successful live spawn did not update queue and history");
    Require(f.Choose()!=&f.A, "successful spawn exclusion is not effective on next preview");
}
void WrapperRoutes() {
    for (int32 competitors : {0,3,4}) {
        Fixture f; f.Game.Competitors=competitors; f.Game.StockResult=&f.B;
        Require(f.Game.ChoosePlayerStart_Implementation(&f.Victim)==&f.B,
            "at-or-below threshold did not preserve stock selection");
        Require(f.Game.StockCalls==1 && f.Game.LegacyCalls==0, "threshold dispatched wrong route");
        Require(!HasLog("spawn emergency"), "intentional small-game route logged emergency");
    }
    {
        Fixture f; f.Game.bSpawnUseNewCTFSelection=false; f.Game.LegacyResult=&f.C;
        Require(f.Game.ChoosePlayerStart_Implementation(&f.Victim)==&f.C,
            "explicit legacy rollback did not preserve legacy result");
        Require(f.Game.LegacyCalls==1 && f.Game.StockCalls==0, "legacy switch dispatched wrong route");
        Require(!HasLog("spawn emergency"), "intentional legacy route logged emergency");
    }
    {
        Fixture f; f.Game.StockResult=&f.B;
        Require(f.Game.ChoosePlayerStart_Implementation(nullptr)==&f.B,
            "null controller did not preserve stock route");
        Require(f.Game.StockCalls==1 && f.Game.LegacyCalls==0, "null controller dispatched wrong route");
        Require(!HasLog("spawn emergency"), "null controller route logged emergency");
    }
}
void WrapperEmergency() {
    for (bool stockHasStart : {false,true}) {
        Fixture f; f.SetPool({}); f.Game.StockResult=stockHasStart ? &f.B : nullptr;
        Require(f.Game.ChoosePlayerStart_Implementation(&f.Victim)==f.Game.StockResult,
            "empty team pool failed to return stock result");
        Require(f.Game.StockCalls==1 && f.Game.LegacyCalls==0, "empty pool dispatched wrong route");
        Require(HasLog("reason=no-usable-team-pool"), "missing-pool protection bypass was not logged");
        Require(HasLog("previousProtection=unavailable"), "missing-pool warning obscures unavailable protection");
        Require(Logs.size()==1, "missing pool should produce one unconditional emergency warning");
    }
}
void WrapperNew() {
    Fixture f; f.Commit(f.Victim,f.A); f.Commit(f.Teammate,f.C); f.Commit(f.Teammate,f.B);
    f.Game.StockResult=&f.A; f.Game.LegacyResult=&f.A;
    Require(f.Game.ChoosePlayerStart_Implementation(&f.Victim)==&f.C,
        "above-threshold wrapper failed to return protected new-selector result");
    Require(f.Game.StockCalls==0 && f.Game.LegacyCalls==0, "normal new selector called a fallback");
    Require(!HasLog("spawn emergency"), "normal protected choice logged emergency");
}

void KillerBeforeFlagPickup() {
    for (bool secondary : {false,true}) for (bool friendlyFirst : {false,true}) {
        Fixture f; f.Game.bIsInstagib=false; f.Game.SpawnMinCycleDistance=0;
        f.Game.bSpawnSecondaryEnabled=secondary; f.Game.SpawnKillerAvoidRadius=2500.f;
        f.Game.SpawnFriendlyBlockRange=6000.f; f.Game.bLogSpawnChoices=true;
        f.VictimPS.LastKillerPlayerState=&f.EnemyPS;
        f.EnemyPawn.Location=FVector(2000,0,0); // Killer has not picked the flag up yet.
        f.B.Location=FVector(5000,0,0); f.SetPool({&f.A,&f.B});
        AUTCharacter friendPawn; friendPawn.Location=f.B.Location; f.Teammate.Pawn=&friendPawn;
        AUTFlag dropped; dropped.Location=f.EnemyPawn.Location;
        AUTCTFFlagBase base; base.MyFlag=&dropped;
        AUTCTFGameState state; state.Bases[0]=&base; state.States[0]=1; f.Game.CTFGameState=&state;
        if (friendlyFirst) f.Game.World.Controllers={{&f.Teammate},{&f.Enemy}};
        else f.Game.World.Controllers={{&f.Enemy},{&f.Teammate}};
        // A wins the old distance fallback (2400 vs 2000) and stock rating.
        f.A.StockRating=100.f; f.B.StockRating=1.f;
        Require(f.Choose()==&f.B, "fallback respawned beside the last killer before flag pickup");
        Require(HasLog(secondary ? "system=secondary" : "system=stock-rating"),
            "friendly block did not exercise the requested fallback");
        Require(!HasLog("no-combat-safe-start"), "safe friend-crowded start was treated as unsafe");
        Require(f.EnemyPawn.CarriedObject==nullptr, "regression accidentally requires flag possession");
    }
}
void CandidateLOS() {
    for (bool reverse : {false,true}) {
        Fixture f; f.Game.bIsInstagib=false; f.Game.SpawnMinCycleDistance=0;
        f.Game.EnemyLOSBlockRange=3000.f; f.Game.SpawnFriendlyBlockRange=3000.f;
        f.B.Location=FVector(1000,1000,0); f.SetPool({&f.A,&f.B});
        f.EnemyPawn.Location=FVector(2000,0,0);
        AUTCharacter friendPawn; friendPawn.Location=f.B.Location; f.Teammate.Pawn=&friendPawn;
        f.Game.World.Controllers=reverse ? std::vector<ControllerRef>{{&f.Enemy},{&f.Teammate}}
            : std::vector<ControllerRef>{{&f.Teammate},{&f.Enemy}};
        // Only A is exposed. Global visibility stubs cannot establish this regression.
        f.Game.World.TraceBlocked=[](const FVector& from, const FVector&) { return from.Y>500.f; };
        Require(f.Choose()==&f.B, "candidate-specific enemy sightline was discarded by fallback");
    }
    {
        Fixture f; f.Game.bIsInstagib=false; f.Game.SpawnMinCycleDistance=0;
        f.Game.EnemyLOSBlockRange=3000.f; f.B.Location=FVector(1000,1000,0); f.SetPool({&f.A,&f.B});
        f.EnemyPawn.Location=FVector(2000,0,0); f.EnemyPawn.RemoteRole=ROLE_AutonomousProxy;
        f.EnemyPawn.Velocity=FVector(1000,0,0); f.EnemyPS.ExactPing=200.f;
        f.Game.World.Controllers={{&f.Enemy}};
        f.Game.World.TraceBlocked=[](const FVector& from, const FVector& to) {
            return from.Y>500.f || to.X>2050.f;
        };
        Require(f.Choose()==&f.B, "blocked predicted-eye trace concealed visible current-eye threat");
    }
}
void CarrierLOS() {
    for (bool secondary : {false,true}) {
        Fixture f; f.Game.bIsInstagib=false; f.Game.SpawnMinCycleDistance=0;
        f.Game.bSpawnSecondaryEnabled=secondary; f.Game.SpawnFriendlyBlockRange=100.f;
        f.Game.EnemyLOSBlockRange=3000.f; f.Game.SpawnFlagCarrierLOSAvoidRadius=3500.f;
        f.EnemyPawn.Location=FVector(3200,0,0); f.B.Location=FVector(6400,0,0); f.SetPool({&f.A,&f.B});
        AUTCharacter friendPawn; friendPawn.Location=f.B.Location; f.Teammate.Pawn=&friendPawn;
        AUTFlag flag; flag.HoldingPawn=&f.EnemyPawn; f.EnemyPawn.CarriedObject=&flag;
        AUTCTFFlagBase base; base.MyFlag=&flag;
        AUTCTFGameState state; state.Bases[0]=&base; state.States[0]=CarriedObjectState::Held;
        f.Game.CTFGameState=&state; f.Game.World.Controllers={{&f.Enemy},{&f.Teammate}};
        f.Game.World.TraceBlocked=[](const FVector& from, const FVector&) { return from.X>5000.f; };
        f.A.StockRating=100.f;
        Require(f.Choose()==&f.B, "fallback dropped the flag carrier's extended LOS avoidance radius");
    }
}
void SafetyCycle() {
    Fixture f; f.Game.bIsInstagib=false;
    f.Game.SpawnEnemyHardRadius=1500.f; f.Game.SpawnEnemyBelowZ=0.f;
    f.EnemyPawn.Location=FVector(500,0,0); f.Game.World.Controllers={{&f.Enemy}};
    f.C.Location=FVector(5000,0,0); // Safe but excluded by the team's cycle tail.
    Require(f.Choose()==&f.C, "unsafe regular candidates beat a safe team-cycle tail start");
    Require(HasLog("reason=no-safe-cycle-start"), "safety cycle relaxation lacked a clear emergency reason");
    Require(!HasLog("no-combat-safe-start"), "safe tail was omitted from threat search");
}
void SafetyHistory() {
    Fixture f; f.Game.bIsInstagib=false; f.Game.SpawnMinCycleDistance=0;
    f.Commit(f.Victim,f.A); f.Commit(f.Teammate,f.C); f.Commit(f.Teammate,f.B);
    f.Game.SpawnEnemyHardRadius=600.f; f.Game.SpawnEnemyBelowZ=0.f;
    f.EnemyPawn.Location=FVector(1500,0,0); f.Game.World.Controllers={{&f.Enemy}};
    auto* chosen=f.Choose();
    Require(chosen && chosen!=&f.A, "safety preference silently re-enabled the player's previous start");
    Require(HasLog("reason=no-combat-safe-start"), "all distinct starts unsafe did not log emergency");
    Require(!HasLog("reason=no-distinct-team-start"), "distinct locations were incorrectly called repeats");
}
void AllThreatened() {
    for (bool secondary : {false,true}) {
        Fixture f; f.Game.bIsInstagib=false; f.Game.bSpawnSecondaryEnabled=secondary;
        f.BlockPrimary();
        Require(f.Choose()!=nullptr, "all unsafe starts prevented respawn");
        Require(HasLog("reason=no-combat-safe-start"), "forced unsafe spawn did not log with logging off");
    }
}
void MixedThreats() {
    for (bool secondary : {false,true}) for (bool saferInTail : {false,true}) {
        Fixture f; f.Game.bIsInstagib=false; f.Game.bSpawnSecondaryEnabled=secondary;
        f.Game.SpawnMinCycleDistance=saferInTail ? 1 : 0;
        f.Game.SpawnEnemyHardRadius=1200.f; f.Game.EnemyLOSBlockRange=3000.f;
        f.Game.SpawnKillerAvoidRadius=2500.f; f.VictimPS.LastKillerPlayerState=&f.EnemyPS;
        f.EnemyPawn.Location=FVector(0,0,0);
        f.A.Location=FVector(1000,0,0); f.C.Location=FVector(-1000,0,0);
        f.B.Location=FVector(0,1500,0); f.SetPool({&f.A,&f.C,&f.B});
        AUTPlayerState extraPS; extraPS.Team=&f.EnemyTeam;
        AUTCharacter extraPawn; extraPawn.Location=FVector(0,2800,0);
        AController extra; extra.PlayerState=&extraPS; extra.Pawn=&extraPawn;
        f.Game.World.Controllers={{&extra},{&f.Enemy}};
        f.Game.World.TraceBlocked=[](const FVector& from, const FVector&) { return from.Y>1000.f; };
        // Every start is inside the last-killer radius. A/C are also exposed
        // and too close. Old distance totals prefer A/C (3000) to B (2800).
        f.A.StockRating=100.f; f.C.StockRating=100.f; f.B.StockRating=1.f;
        Require(f.Choose()==&f.B, "all-unsafe fallback let distance override the less dangerous start");
        Require(HasLog("reason=no-combat-safe-start") && HasLog("threatTier=2"),
            "forced spawn did not explain its remaining killer-proximity risk");
        Require(HasLog("reason=no-safe-cycle-start")==saferInTail,
            "safer unsafe cycle-tail candidate was not handled explicitly");
    }
}
void FloorSeparated() {
    for (bool isKiller : {false,true}) {
        Fixture f; f.Game.bIsInstagib=false; f.Game.SpawnMinCycleDistance=0;
        f.Game.SpawnEnemyHardRadius=1200.f; f.Game.EnemyLOSBlockRange=3000.f;
        f.A.Location=FVector(0,0,300); f.B.Location=FVector(4000,0,0); f.SetPool({&f.A,&f.B});
        f.EnemyPawn.Location=FVector(0,0,0); f.Game.World.Controllers={{&f.Enemy}};
        if (isKiller) { f.VictimPS.LastKillerPlayerState=&f.EnemyPS; f.Game.SpawnKillerAvoidRadius=2500.f; }
        Require(f.Choose()==(isKiller ? &f.B : &f.A),
            "occluded lower floor exception or independent killer protection changed");
    }
}
void ModeScope() {
    for (bool instagib : {false,true}) for (int32 competitors : {2,4,10}) {
        Fixture f; f.Game.bIsInstagib=instagib; f.Game.Competitors=competitors;
        f.Game.StockResult=&f.A; f.Game.SpawnMinCycleDistance=0;
        f.Game.SpawnKillerAvoidRadius=2500.f; f.Game.SpawnFriendlyBlockRange=6000.f;
        f.VictimPS.LastKillerPlayerState=&f.EnemyPS; f.EnemyPawn.Location=FVector(2000,0,0);
        f.B.Location=FVector(5000,0,0); f.SetPool({&f.A,&f.B});
        AUTCharacter friendPawn; friendPawn.Location=f.B.Location; f.Teammate.Pawn=&friendPawn;
        f.Game.World.Controllers={{&f.Enemy},{&f.Teammate}};
        Require(f.Game.ChoosePlayerStart_Implementation(&f.Victim)==(instagib ? &f.A : &f.B),
            "NW small-game safety or original iCTF fallback behavior changed");
        Require(f.Game.StockCalls==(instagib && competitors<=4 ? 1 : 0),
            "mode-specific small-game dispatch is wrong");
        Require(f.Game.LegacyCalls==0, "new-selection mode unexpectedly called legacy");
    }
    Fixture f; f.Game.bIsInstagib=false; f.Game.bSpawnUseNewCTFSelection=false;
    f.Game.Competitors=2; f.Game.LegacyResult=&f.C;
    Require(f.Game.ChoosePlayerStart_Implementation(&f.Victim)==&f.C && f.Game.LegacyCalls==1,
        "NW safety change overrode explicit legacy configuration");
}
void FlagOutRotation() {
    for (bool instagib : {false,true}) for (int32 rotation : {0,1}) {
        Fixture f; f.Game.bIsInstagib=instagib; f.Game.SpawnMinCycleDistance=0;
        f.Game.SpawnRobbedBaseAvoidCount=2.f; f.Game.RobbedSpawnRotation[0]=rotation;
        AUTFlag flag; flag.Location=FVector(9000,0,0);
        AUTCTFFlagBase base; base.MyFlag=&flag; base.Location=f.A.Location;
        AUTCTFGameState state; state.Bases[0]=&base; state.States[0]=1; f.Game.CTFGameState=&state;
        Require(f.Choose()==(instagib && rotation==0 ? &f.B : &f.A),
            "NW flag-out still excludes a near-base start or iCTF rotation changed");
    }
}
void LegacyFlagOutRotation() {
    for (bool instagib : {false,true}) for (int32 rotation : {0,1}) {
        Fixture f; f.Game.bIsInstagib=instagib;
        f.Game.SpawnRobbedBaseAvoidCount=2.f; f.Game.RobbedSpawnRotation[0]=rotation;
        f.A.StockRating=100.f; f.B.StockRating=2.f; f.C.StockRating=1.f;
        AUTFlag flag; flag.Location=FVector(9000,0,0);
        AUTCTFFlagBase base; base.MyFlag=&flag; base.Location=f.A.Location;
        AUTCTFGameState state; state.Bases[0]=&base; state.States[0]=1; f.Game.CTFGameState=&state;
        Require(f.Game.ChooseLegacyPlayerStartNative(&f.Victim)==(instagib && rotation==0 ? &f.B : &f.A),
            "legacy weapon-CTF flag-out exclusion remains or legacy iCTF rotation changed");
        Require(f.Game.RobbedSpawnRotation[0]==rotation+(instagib ? 1 : 0),
            "legacy flag-out exclusion advanced rotation in the wrong modes");
    }
}
int main(int argc, char** argv) {
    if (argc!=2) return 2;
    const std::string which=argv[1];
    if (which=="acrony") Acrony();
    else if (which=="secondary") Secondary();
    else if (which=="positions") Positions();
    else if (which=="invalid-history") InvalidHistory();
    else if (which=="moved-start") MovedStart();
    else if (which=="first") First();
    else if (which=="cycle-tail") CycleTail();
    else if (which=="emergency") Emergency();
    else if (which=="secondary-off") SecondaryOff();
    else if (which=="commit") CommitLifecycle();
    else if (which=="wrapper-routes") WrapperRoutes();
    else if (which=="wrapper-emergency") WrapperEmergency();
    else if (which=="wrapper-new") WrapperNew();
    else if (which=="killer-before-pickup") KillerBeforeFlagPickup();
    else if (which=="candidate-los") CandidateLOS();
    else if (which=="carrier-los") CarrierLOS();
    else if (which=="safety-cycle") SafetyCycle();
    else if (which=="safety-history") SafetyHistory();
    else if (which=="all-threatened") AllThreatened();
    else if (which=="mixed-threats") MixedThreats();
    else if (which=="floor-separated") FloorSeparated();
    else if (which=="mode-scope") ModeScope();
    else if (which=="flag-out-rotation") FlagOutRotation();
    else if (which=="legacy-flag-out") LegacyFlagOutRotation();
    else return 2;
    return 0;
}
