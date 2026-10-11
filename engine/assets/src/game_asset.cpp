#include "foh/game_asset.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>

namespace foh {

namespace {

// ---- the text container ---------------------------------------------------------------------------------------

struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv;
    const std::string* find(std::string_view key) const {
        for (const auto& [k, v] : kv)
            if (k == key) return &v;
        return nullptr;
    }
};

struct Doc {
    std::vector<Section> sections; // [0]: the keys before the first [section]
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parseDoc(std::string_view text, std::string_view expectedType, Doc& doc, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    doc.sections.clear();
    doc.sections.emplace_back();
    bool header = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.front() == '#') continue;
        if (!header) {
            if (!line.starts_with("foh-game-asset")) return fail("not a game asset file (missing the foh-game-asset header)");
            header = true;
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            doc.sections.push_back({std::string(trim(line.substr(1, line.size() - 2))), {}});
            continue;
        }
        const size_t split = line.find_first_of(" \t");
        const std::string_view key = split == std::string_view::npos ? line : line.substr(0, split);
        const std::string_view value = split == std::string_view::npos ? std::string_view{} : trim(line.substr(split));
        doc.sections.back().kv.emplace_back(std::string(key), std::string(value));
    }
    if (!header) return fail("empty file");
    const std::string* type = doc.sections[0].find("type");
    if (!type || *type != expectedType) return fail(std::format("expected a {} definition", expectedType));
    return true;
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

bool parseFloat(std::string_view s, f32& out) {
    s = trim(s);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{};
}

// Reads/writes the same field list: one visitor per type serves both directions, so the two can never disagree.
class Reader {
public:
    static constexpr bool kWriting = false;
    explicit Reader(const Doc& d) : m_doc(d) {}
    void use(const Section* s) { m_cur = s; }
    const Doc& doc() const { return m_doc; }

    void num(const char* key, f32& v) const {
        if (const std::string* s = m_cur->find(key)) {
            f32 parsed;
            if (parseFloat(*s, parsed)) v = parsed;
        }
    }
    void flag(const char* key, bool& v) const {
        if (const std::string* s = m_cur->find(key)) v = *s == "1" || *s == "true";
    }
    void str(const char* key, std::string& v) const {
        if (const std::string* s = m_cur->find(key)) v = *s;
    }
    void vec3(const char* key, Vec3& v) const {
        const std::string* s = m_cur->find(key);
        if (!s) return;
        Vec3 parsed;
        std::string_view rest = *s;
        for (int i = 0; i < 3; ++i) {
            rest = trim(rest);
            const size_t end = rest.find_first_of(" \t");
            if (rest.empty() || !parseFloat(rest.substr(0, end), parsed[i])) return;
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
        }
        v = parsed;
    }
    void list(const char* key, std::vector<f32>& v) const {
        const std::string* s = m_cur->find(key);
        if (!s) return;
        std::vector<f32> parsed;
        std::string_view rest = *s;
        while (!(rest = trim(rest)).empty()) {
            const size_t end = rest.find_first_of(" \t");
            f32 x;
            if (!parseFloat(rest.substr(0, end), x)) return;
            parsed.push_back(x);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
        }
        v = std::move(parsed);
    }
    template <class E>
    void choice(const char* key, E& v, std::initializer_list<const char*> names) const {
        const std::string* s = m_cur->find(key);
        if (!s) return;
        u32 i = 0;
        for (const char* n : names) {
            if (*s == n) {
                v = static_cast<E>(i);
                return;
            }
            ++i;
        }
    }

private:
    const Doc& m_doc;
    const Section* m_cur = nullptr;
};

class Writer {
public:
    static constexpr bool kWriting = true;
    std::string out;
    void begin(const char* section) { out += std::format("\n[{}]\n", section); }
    void num(const char* key, f32& v) { out += std::format("{} {}\n", key, v); }
    void flag(const char* key, bool& v) { out += std::format("{} {}\n", key, v ? 1 : 0); }
    void str(const char* key, std::string& v) {
        if (!v.empty()) out += std::format("{} {}\n", key, oneLine(v));
    }
    void vec3(const char* key, Vec3& v) { out += std::format("{} {} {} {}\n", key, v.x, v.y, v.z); }
    void list(const char* key, std::vector<f32>& v) {
        out += key;
        for (const f32 x : v) out += std::format(" {}", x);
        out += '\n';
    }
    template <class E>
    void choice(const char* key, E& v, std::initializer_list<const char*> names) {
        out += std::format("{} {}\n", key, *(names.begin() + std::min<size_t>(static_cast<size_t>(v), names.size() - 1)));
    }
};

// Header shared by every file. Mutable copies are visited so one function can serve reading and writing.
template <class IO>
void visitWheel(IO& io, WheelDef& w) {
    io.str("name", w.name);
    io.str("node", w.node);
    io.vec3("position", w.position);
    io.num("radius", w.radius);
    io.num("width", w.width);
    io.flag("steered", w.steered);
    io.flag("driven", w.driven);
    io.flag("handbrake", w.handbrake);
}

template <class IO>
void visitVehicle(IO& io, VehicleDef& d) {
    io.str("name", d.name);
    io.str("model", d.model);
    io.num("mass", d.mass);
    io.vec3("centerOfMass", d.centerOfMass);
    io.vec3("colliderCenter", d.colliderCenter);
    io.vec3("colliderHalf", d.colliderHalf);
    io.num("dragCoefficient", d.dragCoefficient);
    io.num("frontalArea", d.frontalArea);
    io.num("downforce", d.downforce);
    io.num("frontFrequency", d.frontFrequency);
    io.num("rearFrequency", d.rearFrequency);
    io.num("dampingRatio", d.dampingRatio);
    io.num("travelUp", d.travelUp);
    io.num("travelDown", d.travelDown);
    io.num("antiRoll", d.antiRoll);
    io.num("frontGrip", d.frontGrip);
    io.num("rearGrip", d.rearGrip);
    io.num("rollingResistance", d.rollingResistance);
    io.num("tractionControl", d.tractionControl);
    io.num("maxTorque", d.maxTorque);
    io.num("idleRpm", d.idleRpm);
    io.num("peakRpm", d.peakRpm);
    io.num("redlineRpm", d.redlineRpm);
    io.list("gears", d.gears);
    io.num("reverseGear", d.reverseGear);
    io.num("finalDrive", d.finalDrive);
    io.num("drivelineEfficiency", d.drivelineEfficiency);
    io.num("brakeTorque", d.brakeTorque);
    io.num("brakeFrontBias", d.brakeFrontBias);
    io.num("handbrakeTorque", d.handbrakeTorque);
    io.num("maxSteerDegrees", d.maxSteerDegrees);
    io.num("highSpeedSteer", d.highSpeedSteer);
    io.num("steerSpeed", d.steerSpeed);
    io.vec3("driverSeat", d.driverSeat);
    io.vec3("exitOffset", d.exitOffset);
    io.num("cameraDistance", d.cameraDistance);
    io.num("cameraHeight", d.cameraHeight);
    io.num("cameraFov", d.cameraFov);
}

template <class IO>
void visitProp(IO& io, PropDef& d) {
    io.str("name", d.name);
    io.str("model", d.model);
    io.choice("body", d.body, {"static", "dynamic", "decoration"});
    io.choice("shape", d.shape, {"box", "sphere", "capsule"});
    io.vec3("center", d.center);
    io.vec3("halfExtents", d.halfExtents);
    io.num("mass", d.mass);
    io.num("friction", d.friction);
    io.num("restitution", d.restitution);
    io.str("surface", d.surface);
    io.flag("breakable", d.breakable);
    io.num("health", d.health);
}

template <class IO>
void visitCharacter(IO& io, CharacterDef& d) {
    io.str("name", d.name);
    io.str("model", d.model);
    io.str("riggedModel", d.riggedModel);
    io.choice("role", d.role, {"player", "npc"});
    io.num("height", d.height);
    io.num("radius", d.radius);
    io.flag("normalizeTPose", d.normalizeTPose);
    io.num("skinSmoothness", d.skinSmoothness);
    io.flag("hasLandmarks", d.hasLandmarks);
}

constexpr const char* kHumanoidNames[kHumanoidJointCount] = {
    "Hips",      "Spine",     "Spine1",       "Spine2",       "Neck",     "Head",       "HeadTop_End", "LeftShoulder", "LeftArm",
    "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand", "LeftUpLeg",   "LeftLeg",      "LeftFoot",
    "LeftToeBase", "LeftToe_End", "RightUpLeg", "RightLeg",   "RightFoot", "RightToeBase", "RightToe_End"};

std::string lowerAscii(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

} // namespace

const char* GameAssetKindName(GameAssetKind kind) {
    switch (kind) {
    case GameAssetKind::Vehicle: return "vehicle";
    case GameAssetKind::Prop: return "prop";
    case GameAssetKind::Character: return "character";
    default: return "none";
    }
}

const char* GameAssetExtension(GameAssetKind kind) {
    switch (kind) {
    case GameAssetKind::Vehicle: return ".fvehicle";
    case GameAssetKind::Prop: return ".fprop";
    case GameAssetKind::Character: return ".fchar";
    default: return "";
    }
}

GameAssetKind GameAssetKindFromPath(std::string_view path) {
    const std::string p = lowerAscii(path);
    for (const GameAssetKind k : {GameAssetKind::Vehicle, GameAssetKind::Prop, GameAssetKind::Character})
        if (p.ends_with(GameAssetExtension(k))) return k;
    return GameAssetKind::None;
}

std::vector<std::string> SplitWheelNodes(std::string_view nodes) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= nodes.size()) {
        size_t end = nodes.find('|', pos);
        if (end == std::string_view::npos) end = nodes.size();
        const std::string_view part = trim(nodes.substr(pos, end - pos));
        if (!part.empty()) out.emplace_back(part);
        pos = end + 1;
    }
    return out;
}

const char* HumanoidJointName(u32 index) { return index < kHumanoidJointCount ? kHumanoidNames[index] : ""; }

i32 HumanoidJointIndex(std::string_view name) {
    for (u32 i = 0; i < kHumanoidJointCount; ++i)
        if (name == kHumanoidNames[i]) return static_cast<i32>(i);
    return -1;
}

// ---- vehicle ---------------------------------------------------------------------------------------------------

std::string WriteVehicleDef(const VehicleDef& def) {
    VehicleDef d = def;
    Writer w;
    w.out = "foh-game-asset 1\ntype vehicle\n";
    visitVehicle(w, d);
    for (WheelDef wheel : d.wheels) {
        w.begin("wheel");
        visitWheel(w, wheel);
    }
    return std::move(w.out);
}

bool ParseVehicleDef(std::string_view text, VehicleDef& out, std::string* error) {
    Doc doc;
    if (!parseDoc(text, "vehicle", doc, error)) return false;
    VehicleDef d;
    Reader r(doc);
    r.use(&doc.sections[0]);
    visitVehicle(r, d);
    for (const Section& s : doc.sections) {
        if (s.name != "wheel") continue;
        if (d.wheels.size() >= kMaxVehicleWheels) break;
        WheelDef w;
        r.use(&s);
        visitWheel(r, w);
        d.wheels.push_back(std::move(w));
    }
    if (d.gears.size() > kMaxVehicleGears) d.gears.resize(kMaxVehicleGears);
    out = std::move(d);
    return true;
}

std::vector<std::string> ValidateVehicleDef(const VehicleDef& d) {
    std::vector<std::string> problems;
    if (d.model.empty()) problems.push_back("The vehicle has no model.");
    if (d.wheels.size() < 3) problems.push_back("A vehicle needs at least 3 wheels (4 for a car).");
    if (d.wheels.size() > kMaxVehicleWheels) problems.push_back(std::format("At most {} wheels are supported.", kMaxVehicleWheels));
    bool steered = false, driven = false;
    for (const WheelDef& w : d.wheels) {
        steered = steered || w.steered;
        driven = driven || w.driven;
        if (w.radius < 0.05f || w.radius > 1.5f) problems.push_back(std::format("Wheel '{}' has an implausible radius ({} m).", w.name, w.radius));
    }
    if (!d.wheels.empty() && !steered) problems.push_back("No wheel is steered.");
    if (!d.wheels.empty() && !driven) problems.push_back("No wheel is driven: the vehicle cannot accelerate.");
    if (d.mass < 50.0f || d.mass > 60000.0f) problems.push_back(std::format("The mass ({} kg) looks wrong.", d.mass));
    if (d.gears.empty()) problems.push_back("The transmission has no forward gears.");
    if (d.redlineRpm <= d.idleRpm) problems.push_back("The redline must be above the idle rpm.");
    if (d.travelUp + d.travelDown < 0.04f) problems.push_back("The suspension travel is almost zero.");
    if (d.wheels.size() >= 3) {
        f32 lo = d.wheels[0].position.y, hi = lo;
        for (const WheelDef& w : d.wheels) {
            lo = std::min(lo, w.position.y);
            hi = std::max(hi, w.position.y);
        }
        if (hi - lo > 0.35f) problems.push_back("The wheels are at very different heights.");
        if (d.centerOfMass.y > hi + 1.2f) problems.push_back("The center of mass is far above the wheels: the vehicle will roll over.");
    }
    return problems;
}

// ---- prop --------------------------------------------------------------------------------------------------------

std::string WritePropDef(const PropDef& def) {
    PropDef d = def;
    Writer w;
    w.out = "foh-game-asset 1\ntype prop\n";
    visitProp(w, d);
    return std::move(w.out);
}

bool ParsePropDef(std::string_view text, PropDef& out, std::string* error) {
    Doc doc;
    if (!parseDoc(text, "prop", doc, error)) return false;
    PropDef d;
    Reader r(doc);
    r.use(&doc.sections[0]);
    visitProp(r, d);
    out = std::move(d);
    return true;
}

// ---- character ---------------------------------------------------------------------------------------------------

std::string WriteCharacterDef(const CharacterDef& def) {
    CharacterDef d = def;
    Writer w;
    w.out = "foh-game-asset 1\ntype character\n";
    visitCharacter(w, d);
    if (d.hasLandmarks) {
        w.begin("landmarks");
        for (u32 i = 0; i < kHumanoidJointCount; ++i) w.vec3(kHumanoidNames[i], d.landmarks[i]);
    }
    return std::move(w.out);
}

bool ParseCharacterDef(std::string_view text, CharacterDef& out, std::string* error) {
    Doc doc;
    if (!parseDoc(text, "character", doc, error)) return false;
    CharacterDef d;
    Reader r(doc);
    r.use(&doc.sections[0]);
    visitCharacter(r, d);
    for (const Section& s : doc.sections) {
        if (s.name != "landmarks") continue;
        r.use(&s);
        for (u32 i = 0; i < kHumanoidJointCount; ++i) r.vec3(kHumanoidNames[i], d.landmarks[i]);
    }
    out = std::move(d);
    return true;
}

} // namespace foh
