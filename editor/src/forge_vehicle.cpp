// The vehicle tab of the Asset Forge: finds the wheels, proposes a setup from the size of the model, and edits every number
// the driving simulation (game/src/vehicle_dynamics.cpp) reads.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

#include "asset_forge.h"
#include "foh/log.h"
#include "foh/scene/game_asset_request.h"

namespace foh {

namespace {

constexpr ImU32 kAxleColor[] = {IM_COL32(90, 200, 255, 255), IM_COL32(120, 230, 150, 255)};
constexpr ImU32 kWarn = IM_COL32(255, 200, 80, 255);

struct Preset {
    const char* name;
    f32 torquePerKg, redline, peak, grip, frequency, damping, antiRoll, steerDegrees, drag, downforce;
    int layout; // 0 FWD, 1 RWD, 2 AWD
    std::vector<f32> gears;
    f32 finalDrive;
};

const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"Hatchback (front wheel drive)", 0.17f, 6200, 3800, 1.05f, 1.5f, 0.65f, 0.30f, 36, 0.33f, 0.0f, 0, {3.6f, 2.1f, 1.4f, 1.05f, 0.85f}, 4.0f},
        {"Sedan (rear wheel drive)", 0.24f, 6500, 4200, 1.10f, 1.6f, 0.60f, 0.35f, 34, 0.32f, 0.0f, 1, {3.55f, 2.2f, 1.5f, 1.12f, 0.9f, 0.74f}, 3.7f},
        {"Sports car", 0.42f, 8200, 5800, 1.35f, 2.3f, 0.70f, 0.55f, 30, 0.30f, 0.35f, 1, {3.2f, 2.1f, 1.55f, 1.22f, 1.0f, 0.82f}, 3.7f},
        {"Muscle car", 0.42f, 6000, 3600, 1.15f, 1.4f, 0.50f, 0.25f, 32, 0.38f, 0.0f, 1, {2.7f, 1.9f, 1.4f, 1.0f, 0.8f}, 3.4f},
        {"SUV / pickup (all wheel drive)", 0.20f, 5500, 3200, 1.00f, 1.2f, 0.70f, 0.45f, 34, 0.40f, 0.0f, 2, {3.9f, 2.3f, 1.5f, 1.0f, 0.8f, 0.65f}, 4.1f},
        {"Van / truck", 0.13f, 4800, 2800, 0.95f, 1.1f, 0.70f, 0.50f, 38, 0.50f, 0.0f, 1, {5.0f, 2.9f, 1.7f, 1.0f, 0.75f}, 4.5f},
        {"Old beater (soft, slow)", 0.15f, 5200, 3000, 0.95f, 1.0f, 0.45f, 0.20f, 36, 0.38f, 0.0f, 0, {3.7f, 2.0f, 1.3f, 0.95f}, 4.2f},
    };
    return p;
}

bool containsAny(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

std::string joinPaths(const std::vector<std::string>& paths) {
    std::string out;
    for (const std::string& p : paths) out += (out.empty() ? "" : "|") + p;
    return out;
}

} // namespace

// ---- automatic setup -----------------------------------------------------------------------------------------------

void AssetForge::detectWheels() {
    m_vehicleNote.clear();
    if (!m_haveBounds) {
        m_vehicleNote = "The meshes are still being read.";
        return;
    }
    struct Candidate {
        int node;
        Vec3 lo, hi, c;
    };
    auto isWheelName = [&](const std::string& name) {
        return containsAny(name, {"wheel", "tire", "tyre", "rim", "roda", "pneu", "rueda", "llanta", "felge", "wiel", "jante"}) &&
               !containsAny(name, {"steer", "spare", "arch", "well", "cover", "fender", "brake", "disc", "caliper", "guard", "wipe"});
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<Candidate> candidates;
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            const Node& n = m_nodes[i];
            if (!n.hasBounds) continue;
            const i32 parent = m_work.nodes[i].parent;
            if (!isWheelName(n.lowerName) && !(parent >= 0 && isWheelName(m_nodes[static_cast<size_t>(parent)].lowerName))) continue;
            candidates.push_back({static_cast<int>(i), n.lo, n.hi, 0.5f * (n.lo + n.hi)});
        }
        // Which way do the axles point? A wheel is a squat disc: its smallest extent is the axle.
        int axleX = 0, axleZ = 0;
        for (const Candidate& c : candidates) {
            const Vec3 d = c.hi - c.lo;
            if (d.y < 0.05f) continue;
            if (d.x < d.z * 0.85f) ++axleX;
            else if (d.z < d.x * 0.85f) ++axleZ;
        }
        if (attempt == 0 && axleZ > axleX && axleZ >= 3) {
            // Turned sideways: a vehicle points along +Z with its axles along X.
            m_fixQuarterTurns = (m_fixQuarterTurns + 1) & 3;
            rebuildWork();
            m_previewDirty = true;
            m_vehicleNote = "The wheel axles pointed along Z: the model was turned 90 degrees. Check that the front points toward +Z (the green arrow); use 'Turn 180' if not.";
            continue;
        }

        std::vector<Candidate> round;
        for (const Candidate& c : candidates) {
            const Vec3 d = c.hi - c.lo;
            if (d.y < 0.15f || d.y > 1.8f) continue;
            if (d.x > d.z * 0.95f) continue;                           // not a disc with an axle along X
            if (std::abs(d.y - d.z) > 0.4f * std::max(d.y, d.z)) continue; // not round
            round.push_back(c);
        }
        // Merge the pieces of one wheel (a tire and a rim have almost the same centre).
        std::vector<int> group(round.size());
        for (size_t i = 0; i < group.size(); ++i) group[i] = static_cast<int>(i);
        auto find = [&](int a) {
            while (group[static_cast<size_t>(a)] != a) a = group[static_cast<size_t>(a)] = group[static_cast<size_t>(group[static_cast<size_t>(a)])];
            return a;
        };
        for (size_t i = 0; i < round.size(); ++i)
            for (size_t j = i + 1; j < round.size(); ++j) {
                const Vec3 a = round[i].c, b = round[j].c;
                const f32 size = std::max(round[i].hi.y - round[i].lo.y, round[j].hi.y - round[j].lo.y);
                if (std::abs(a.x - b.x) < 0.3f * size + 0.12f && std::abs(a.y - b.y) < 0.2f * size && std::abs(a.z - b.z) < 0.2f * size)
                    group[static_cast<size_t>(find(static_cast<int>(i)))] = find(static_cast<int>(j));
            }
        struct Wheel {
            Vec3 lo{FLT_MAX}, hi{-FLT_MAX};
            std::vector<std::string> nodes;
        };
        std::vector<std::pair<int, Wheel>> wheels;
        for (size_t i = 0; i < round.size(); ++i) {
            const int root = find(static_cast<int>(i));
            auto it = std::find_if(wheels.begin(), wheels.end(), [&](const auto& w) { return w.first == root; });
            if (it == wheels.end()) {
                wheels.push_back({root, {}});
                it = wheels.end() - 1;
            }
            it->second.lo = glm::min(it->second.lo, round[i].lo);
            it->second.hi = glm::max(it->second.hi, round[i].hi);
            it->second.nodes.push_back(m_nodes[static_cast<size_t>(round[i].node)].path);
        }
        // Drop the odd ones out (a spare tire, a steering wheel): keep those whose size matches the median.
        if (wheels.size() > 4) {
            std::vector<f32> sizes;
            for (const auto& w : wheels) sizes.push_back(w.second.hi.y - w.second.lo.y);
            std::sort(sizes.begin(), sizes.end());
            const f32 median = sizes[sizes.size() / 2];
            wheels.erase(std::remove_if(wheels.begin(), wheels.end(),
                                        [&](const auto& w) {
                                            const f32 s = w.second.hi.y - w.second.lo.y;
                                            return s < 0.7f * median || s > 1.4f * median;
                                        }),
                         wheels.end());
        }
        if (wheels.size() < 3 || wheels.size() > kMaxVehicleWheels) {
            layoutWheelsFromBounds();
            m_vehicleNote = (m_vehicleNote.empty() ? "" : m_vehicleNote + " ") +
                            std::format("Found {} wheel-like part(s) by name and shape, not enough: the wheels were placed from the size of the model. Move them with the gizmo.", wheels.size());
            return;
        }
        // Front first, left (+X) first.
        std::sort(wheels.begin(), wheels.end(), [](const auto& a, const auto& b) {
            const f32 za = 0.5f * (a.second.lo.z + a.second.hi.z), zb = 0.5f * (b.second.lo.z + b.second.hi.z);
            if (std::abs(za - zb) > 0.25f) return za > zb;
            return 0.5f * (a.second.lo.x + a.second.hi.x) > 0.5f * (b.second.lo.x + b.second.hi.x);
        });
        m_vehicle.wheels.clear();
        for (size_t i = 0; i < wheels.size(); ++i) {
            const Wheel& w = wheels[i].second;
            WheelDef def;
            def.position = 0.5f * (w.lo + w.hi);
            def.radius = 0.25f * ((w.hi.y - w.lo.y) + (w.hi.z - w.lo.z));
            def.width = w.hi.x - w.lo.x;
            std::vector<std::string> nodes = w.nodes;
            if (nodes.size() > kMaxWheelNodes) nodes.resize(kMaxWheelNodes);
            def.node = joinPaths(nodes);
            m_vehicle.wheels.push_back(def);
        }
        // Names, and a centred origin: the middle between the wheels, on the ground.
        Vec3 centroid(0.0f);
        f32 bottom = FLT_MAX;
        for (const WheelDef& w : m_vehicle.wheels) {
            centroid += w.position;
            bottom = std::min(bottom, w.position.y - w.radius);
        }
        centroid /= static_cast<f32>(m_vehicle.wheels.size());
        const Vec3 shift(-centroid.x, -bottom, -centroid.z);
        m_fixOffset += shift;
        rebuildWork();
        for (WheelDef& w : m_vehicle.wheels) w.position += shift;
        m_previewDirty = true;
        const bool four = m_vehicle.wheels.size() == 4;
        for (size_t i = 0; i < m_vehicle.wheels.size(); ++i) {
            WheelDef& w = m_vehicle.wheels[i];
            w.name = four ? std::string(i < 2 ? "Front " : "Rear ") + (w.position.x > 0.0f ? "Left" : "Right") : std::format("Wheel {}", i + 1);
        }
        setDriveLayout(m_vehicle.wheels.size() >= 4 ? 1 : 0);
        estimateVehicleFromBounds();
        m_vehicleDirty = true;
        m_vehicleNote += std::format(" Found {} wheels from the model.", m_vehicle.wheels.size());
        return;
    }
}

void AssetForge::layoutWheelsFromBounds() {
    Vec3 lo, hi;
    if (!boundsOfModel(lo, hi)) {
        lo = Vec3(-0.9f, 0.0f, -2.2f);
        hi = Vec3(0.9f, 1.5f, 2.2f);
    }
    const f32 length = hi.z - lo.z, width = hi.x - lo.x;
    const f32 radius = std::clamp(0.075f * length, 0.15f, 0.7f);
    const f32 track = 0.5f * width * 0.86f;
    const f32 zc = 0.5f * (lo.z + hi.z);
    m_vehicle.wheels.clear();
    const f32 zs[2] = {zc + 0.30f * length, zc - 0.28f * length};
    const char* axle[2] = {"Front ", "Rear "};
    for (int a = 0; a < 2; ++a)
        for (int s = 0; s < 2; ++s) {
            WheelDef w;
            w.name = std::string(axle[a]) + (s == 0 ? "Left" : "Right");
            w.position = Vec3(s == 0 ? track : -track, lo.y + radius, zs[a]);
            w.radius = radius;
            w.width = std::clamp(0.13f * width, 0.12f, 0.4f);
            m_vehicle.wheels.push_back(w);
        }
    setDriveLayout(1);
    estimateVehicleFromBounds();
    m_vehicleDirty = true;
}

void AssetForge::estimateVehicleFromBounds() {
    Vec3 lo, hi;
    if (!boundsOfModel(lo, hi)) return;
    const f32 length = hi.z - lo.z, width = hi.x - lo.x, height = std::max(hi.y - std::max(lo.y, 0.0f), 0.5f);
    f32 radius = 0.33f, bottom = 0.0f;
    if (!m_vehicle.wheels.empty()) {
        radius = 0.0f;
        bottom = FLT_MAX;
        for (const WheelDef& w : m_vehicle.wheels) {
            radius += w.radius;
            bottom = std::min(bottom, w.position.y - w.radius);
        }
        radius /= static_cast<f32>(m_vehicle.wheels.size());
    }
    const f32 top = hi.y;
    m_vehicle.colliderCenter = Vec3(0.5f * (lo.x + hi.x), 0.5f * (bottom + 0.15f + top), 0.5f * (lo.z + hi.z));
    m_vehicle.colliderHalf = Vec3(0.5f * width * 0.98f, 0.5f * (top - bottom - 0.15f), 0.5f * length);
    m_vehicle.mass = std::clamp(110.0f * length * width * height, 400.0f, 14000.0f);
    m_vehicle.centerOfMass = Vec3(0.0f, std::clamp(0.34f * height, radius, 0.55f * height), 0.02f * length);
    m_vehicle.travelUp = std::clamp(0.4f * radius, 0.06f, 0.3f);
    m_vehicle.travelDown = std::clamp(0.45f * radius, 0.06f, 0.35f);
    m_vehicle.maxTorque = m_vehicle.mass * 0.24f;
    m_vehicle.brakeTorque = m_vehicle.mass * 2.0f;
    m_vehicle.handbrakeTorque = m_vehicle.brakeTorque * 1.25f;
    m_vehicle.frontalArea = std::clamp(0.8f * width * height, 1.0f, 12.0f);
    m_vehicle.driverSeat = Vec3(width * 0.22f, bottom + 0.5f * height, 0.05f * length);
    m_vehicle.exitOffset = Vec3(0.5f * width + 0.9f, 0.0f, 0.0f);
    m_vehicle.cameraDistance = std::clamp(1.45f * length, 3.0f, 14.0f);
    m_vehicle.cameraHeight = std::clamp(height + 0.55f, 1.6f, 6.0f);
}

void AssetForge::applyPreset(int index) {
    const auto& list = presets();
    if (index < 0 || index >= static_cast<int>(list.size())) return;
    const Preset& p = list[static_cast<size_t>(index)];
    m_vehicle.maxTorque = m_vehicle.mass * p.torquePerKg;
    m_vehicle.redlineRpm = p.redline;
    m_vehicle.peakRpm = p.peak;
    m_vehicle.frontGrip = p.grip;
    m_vehicle.rearGrip = p.grip * 1.04f;
    m_vehicle.frontFrequency = p.frequency;
    m_vehicle.rearFrequency = p.frequency * 1.06f;
    m_vehicle.dampingRatio = p.damping;
    m_vehicle.antiRoll = p.antiRoll;
    m_vehicle.maxSteerDegrees = p.steerDegrees;
    m_vehicle.dragCoefficient = p.drag;
    m_vehicle.downforce = p.downforce;
    m_vehicle.gears = p.gears;
    m_vehicle.finalDrive = p.finalDrive;
    setDriveLayout(p.layout);
    m_vehicleDirty = true;
}

void AssetForge::setDriveLayout(int layout) {
    if (m_vehicle.wheels.empty()) return;
    f32 minZ = FLT_MAX, maxZ = -FLT_MAX;
    for (const WheelDef& w : m_vehicle.wheels) {
        minZ = std::min(minZ, w.position.z);
        maxZ = std::max(maxZ, w.position.z);
    }
    const f32 tolerance = 0.1f * (maxZ - minZ);
    for (WheelDef& w : m_vehicle.wheels) {
        const bool front = w.position.z >= maxZ - tolerance, rear = w.position.z <= minZ + tolerance;
        w.steered = front;
        w.driven = layout == 2 || (layout == 0 && front) || (layout == 1 && rear);
        w.handbrake = rear;
    }
    m_vehicleDirty = true;
}

void AssetForge::mirrorWheel(int index) {
    if (index < 0 || index >= static_cast<int>(m_vehicle.wheels.size())) return;
    WheelDef& a = m_vehicle.wheels[static_cast<size_t>(index)];
    int best = -1;
    f32 bestD = FLT_MAX;
    for (size_t j = 0; j < m_vehicle.wheels.size(); ++j) {
        if (static_cast<int>(j) == index) continue;
        const WheelDef& b = m_vehicle.wheels[j];
        if (b.position.x * a.position.x >= 0.0f) continue;
        const f32 d = std::abs(b.position.z - a.position.z) + 0.2f * std::abs(b.position.y - a.position.y);
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(j);
        }
    }
    if (best < 0 || bestD > 0.8f) return;
    WheelDef& b = m_vehicle.wheels[static_cast<size_t>(best)];
    b.position = Vec3(-a.position.x, a.position.y, a.position.z);
    b.radius = a.radius;
    b.width = a.width;
}

bool AssetForge::saveVehicle() {
    m_vehicle.model = m_sourcePath;
    const std::vector<std::string> problems = ValidateVehicleDef(m_vehicle);
    if (!writeText(pathFor(".fvehicle"), WriteVehicleDef(m_vehicle))) return false;
    m_vehicleLoaded = true;
    m_vehicleDirty = false;
    if (m_host.refreshBrowser) m_host.refreshBrowser();
    if (m_host.log) m_host.log(std::format("Saved vehicle '{}'{}", pathFor(".fvehicle"), problems.empty() ? "" : std::format(" ({} warning(s))", problems.size())));
    return true;
}

// ---- UI --------------------------------------------------------------------------------------------------------------

void AssetForge::drawVehicleTab() {
    VehicleDef& v = m_vehicle;
    bool changed = false;
    auto drag = [&](const char* label, f32& value, f32 speed, f32 lo, f32 hi, const char* fmt = "%.3f") {
        const bool c = ImGui::DragFloat(label, &value, speed, lo, hi, fmt);
        changed = changed || c;
        return c;
    };

    if (m_vehicleLoaded) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s", pathFor(".fvehicle").c_str());
    else ImGui::TextDisabled("Not saved yet: the numbers below are proposed from the size of the model.");
    if (!m_vehicleNote.empty()) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "%s", m_vehicleNote.c_str());

    ImGui::SeparatorText("Automatic setup");
    if (ImGui::Button("Find the wheels")) detectWheels();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Looks for meshes called wheel / tire / rim / roda... shaped like discs and groups the pieces of each wheel.\nIt also turns the model if the axles point along Z and puts the origin on the ground between the wheels.");
    ImGui::SameLine();
    if (ImGui::Button("Wheels from size")) layoutWheelsFromBounds();
    ImGui::SameLine();
    if (ImGui::Button("Body from size")) {
        estimateVehicleFromBounds();
        m_vehicleDirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collider box, mass, centre of mass, brakes and camera from the dimensions.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15.0f);
    const auto& list = presets();
    if (ImGui::BeginCombo("##preset", list[static_cast<size_t>(m_preset)].name)) {
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (ImGui::Selectable(list[static_cast<size_t>(i)].name, i == m_preset)) m_preset = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply preset")) applyPreset(m_preset);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Engine, gearbox, tires, springs and steering of a kind of car, scaled to this mass.");

    const std::vector<std::string> problems = ValidateVehicleDef(v);
    for (const std::string& p : problems) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "! %s", p.c_str());

    // ---- wheels ----
    ImGui::SeparatorText("Wheels");
    for (int i = 0; i < static_cast<int>(v.wheels.size()); ++i) {
        ImGui::PushID(i);
        if (i) ImGui::SameLine();
        if (ImGui::Selectable(v.wheels[static_cast<size_t>(i)].name.empty() ? std::format("Wheel {}", i + 1).c_str() : v.wheels[static_cast<size_t>(i)].name.c_str(),
                              m_selectedWheel == i && m_vehicleHandle == VehicleHandle::Wheel, 0, ImVec2(ImGui::GetFontSize() * 6.5f, 0))) {
            m_selectedWheel = i;
            m_vehicleHandle = VehicleHandle::Wheel;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add wheel") && v.wheels.size() < kMaxVehicleWheels) {
        WheelDef w = v.wheels.empty() ? WheelDef{} : v.wheels.back();
        w.name = std::format("Wheel {}", v.wheels.size() + 1);
        w.node.clear();
        v.wheels.push_back(w);
        m_selectedWheel = static_cast<int>(v.wheels.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(v.wheels.empty());
    if (ImGui::Button("Remove") && !v.wheels.empty()) {
        v.wheels.erase(v.wheels.begin() + std::clamp(m_selectedWheel, 0, static_cast<int>(v.wheels.size()) - 1));
        m_selectedWheel = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Mirror left / right", &m_mirrorWheels);
    if (!v.wheels.empty()) {
        m_selectedWheel = std::clamp(m_selectedWheel, 0, static_cast<int>(v.wheels.size()) - 1);
        WheelDef& w = v.wheels[static_cast<size_t>(m_selectedWheel)];
        bool wheelChanged = false;
        char name[48];
        std::snprintf(name, sizeof(name), "%s", w.name.c_str());
        if (ImGui::InputText("Name", name, sizeof(name))) {
            w.name = name;
            changed = true;
        }
        wheelChanged |= ImGui::DragFloat3("Centre", &w.position.x, 0.005f, -20.0f, 20.0f, "%.3f");
        wheelChanged |= ImGui::DragFloat("Radius", &w.radius, 0.002f, 0.05f, 1.5f, "%.3f m");
        wheelChanged |= ImGui::DragFloat("Width", &w.width, 0.002f, 0.02f, 1.0f, "%.3f m");
        changed |= ImGui::Checkbox("Steered", &w.steered);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Driven", &w.driven);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Handbrake", &w.handbrake);
        if (wheelChanged && m_mirrorWheels) mirrorWheel(m_selectedWheel);
        changed |= wheelChanged;

        // The meshes that turn with this wheel.
        ImGui::TextUnformatted("Meshes");
        ImGui::SameLine();
        const std::vector<std::string> nodes = SplitWheelNodes(w.node);
        if (ImGui::SmallButton(nodes.empty() ? "choose..." : std::format("{} part(s)...", nodes.size()).c_str())) ImGui::OpenPopup("##wheelnodes");
        if (ImGui::BeginPopup("##wheelnodes")) {
            static char filter[48] = {};
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            ImGui::InputTextWithHint("##f", "filter", filter, sizeof(filter));
            std::string needle = filter;
            for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ImGui::BeginChild("##nl", ImVec2(ImGui::GetFontSize() * 24.0f, ImGui::GetFontSize() * 14.0f))) {
                for (size_t i = 0; i < m_nodes.size(); ++i) {
                    if (!m_work.nodes[i].mesh.isValid()) continue;
                    if (!needle.empty() && m_nodes[i].lowerName.find(needle) == std::string::npos) continue;
                    const bool on = std::find(nodes.begin(), nodes.end(), m_nodes[i].path) != nodes.end();
                    if (ImGui::Selectable(m_nodes[i].path.c_str(), on, ImGuiSelectableFlags_DontClosePopups)) {
                        std::vector<std::string> next = nodes;
                        if (on) next.erase(std::find(next.begin(), next.end(), m_nodes[i].path));
                        else if (next.size() < kMaxWheelNodes) next.push_back(m_nodes[i].path);
                        w.node = joinPaths(next);
                        changed = true;
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        for (const std::string& n : nodes) ImGui::BulletText("%s", n.c_str());
        if (nodes.empty()) ImGui::TextDisabled("No mesh: the wheel is simulated but not drawn turning (the body mesh may include it).");
    }
    ImGui::TextUnformatted("Drive:");
    ImGui::SameLine();
    if (ImGui::SmallButton("Front")) setDriveLayout(0);
    ImGui::SameLine();
    if (ImGui::SmallButton("Rear")) setDriveLayout(1);
    ImGui::SameLine();
    if (ImGui::SmallButton("All wheels")) setDriveLayout(2);

    // ---- gizmo selection ----
    ImGui::SeparatorText("Handles in the viewport");
    int handle = static_cast<int>(m_vehicleHandle);
    const char* handleNames[] = {"Wheel", "Center of mass", "Collider", "Driver seat", "Exit door"};
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(handleNames[i], &handle, i)) m_vehicleHandle = static_cast<VehicleHandle>(handle);
        if (i == 2) ImGui::NewLine();
    }

    if (ImGui::CollapsingHeader("Body", ImGuiTreeNodeFlags_DefaultOpen)) {
        drag("Mass##v", v.mass, 5.0f, 50.0f, 60000.0f, "%.0f kg");
        changed |= ImGui::DragFloat3("Center of mass", &v.centerOfMass.x, 0.01f, -5.0f, 5.0f, "%.2f");
        changed |= ImGui::DragFloat3("Collider centre", &v.colliderCenter.x, 0.01f, -10.0f, 10.0f, "%.2f");
        changed |= ImGui::DragFloat3("Collider half size", &v.colliderHalf.x, 0.01f, 0.1f, 10.0f, "%.2f");
        drag("Drag coefficient", v.dragCoefficient, 0.005f, 0.0f, 2.0f, "%.2f");
        drag("Frontal area", v.frontalArea, 0.05f, 0.5f, 20.0f, "%.2f m2");
        drag("Downforce", v.downforce, 0.01f, 0.0f, 5.0f, "%.2f N/(m/s)2");
    }
    if (ImGui::CollapsingHeader("Suspension")) {
        drag("Front frequency", v.frontFrequency, 0.01f, 0.4f, 6.0f, "%.2f Hz");
        drag("Rear frequency", v.rearFrequency, 0.01f, 0.4f, 6.0f, "%.2f Hz");
        drag("Damping ratio", v.dampingRatio, 0.005f, 0.1f, 1.5f, "%.2f");
        drag("Travel up", v.travelUp, 0.002f, 0.01f, 0.6f, "%.3f m");
        drag("Travel down", v.travelDown, 0.002f, 0.01f, 0.6f, "%.3f m");
        drag("Anti-roll bar", v.antiRoll, 0.005f, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Frequency is how bouncy the body is (1 soft, 2+ sporty). Damping 0.6 is a good ride.");
    }
    if (ImGui::CollapsingHeader("Tires")) {
        drag("Front grip", v.frontGrip, 0.005f, 0.3f, 2.5f, "%.2f");
        drag("Rear grip", v.rearGrip, 0.005f, 0.3f, 2.5f, "%.2f");
        drag("Rolling resistance", v.rollingResistance, 0.0005f, 0.0f, 0.1f, "%.4f");
        drag("Traction control", v.tractionControl, 0.005f, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Grip is the friction coefficient on dry asphalt: 0.9 old tires, 1.1 normal, 1.4 racing.");
    }
    if (ImGui::CollapsingHeader("Engine and gearbox")) {
        drag("Max torque", v.maxTorque, 1.0f, 20.0f, 3000.0f, "%.0f Nm");
        drag("Idle", v.idleRpm, 5.0f, 300.0f, 2000.0f, "%.0f rpm");
        drag("Torque peak", v.peakRpm, 10.0f, 1000.0f, 12000.0f, "%.0f rpm");
        drag("Redline", v.redlineRpm, 10.0f, 2000.0f, 14000.0f, "%.0f rpm");
        ImGui::TextUnformatted("Gears");
        for (size_t i = 0; i < v.gears.size(); ++i) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
            changed |= ImGui::DragFloat(std::format("##g{}", i).c_str(), &v.gears[i], 0.01f, 0.2f, 8.0f, "%.2f");
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("+") && v.gears.size() < kMaxVehicleGears) {
            v.gears.push_back(std::max(v.gears.empty() ? 1.0f : v.gears.back() * 0.82f, 0.3f));
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("-") && v.gears.size() > 1) {
            v.gears.pop_back();
            changed = true;
        }
        drag("Reverse gear", v.reverseGear, 0.01f, 0.5f, 8.0f, "%.2f");
        drag("Final drive", v.finalDrive, 0.01f, 1.0f, 8.0f, "%.2f");
        drag("Driveline efficiency", v.drivelineEfficiency, 0.005f, 0.5f, 1.0f, "%.2f");
        // Top speed at the redline in the last gear (what the engine allows; drag lowers it).
        f32 radius = v.wheels.empty() ? 0.33f : 0.0f;
        u32 n = 0;
        for (const WheelDef& w : v.wheels)
            if (w.driven) {
                radius += w.radius;
                ++n;
            }
        if (n) radius /= static_cast<f32>(n);
        if (!v.gears.empty())
            ImGui::TextDisabled("Top speed by the gearing: %.0f km/h", v.redlineRpm * 6.2831853f / 60.0f / (v.gears.back() * v.finalDrive) * radius * 3.6f);
    }
    if (ImGui::CollapsingHeader("Brakes and steering")) {
        drag("Brake torque", v.brakeTorque, 10.0f, 100.0f, 20000.0f, "%.0f Nm per wheel");
        drag("Front brake bias", v.brakeFrontBias, 0.005f, 0.2f, 0.9f, "%.2f");
        drag("Handbrake torque", v.handbrakeTorque, 10.0f, 100.0f, 20000.0f, "%.0f Nm");
        drag("Max steering angle", v.maxSteerDegrees, 0.1f, 5.0f, 60.0f, "%.1f deg");
        drag("Steering at 40 m/s", v.highSpeedSteer, 0.005f, 0.05f, 1.0f, "%.2f of full");
        drag("Steering speed", v.steerSpeed, 0.02f, 0.5f, 12.0f, "%.1f /s");
    }
    if (ImGui::CollapsingHeader("Driver and camera")) {
        changed |= ImGui::DragFloat3("Driver seat", &v.driverSeat.x, 0.01f, -5.0f, 5.0f, "%.2f");
        changed |= ImGui::DragFloat3("Exit position", &v.exitOffset.x, 0.01f, -6.0f, 6.0f, "%.2f");
        drag("Camera distance", v.cameraDistance, 0.05f, 1.0f, 30.0f, "%.1f m");
        drag("Camera height", v.cameraHeight, 0.05f, 0.5f, 12.0f, "%.1f m");
        drag("Camera field of view", v.cameraFov, 0.005f, 0.5f, 1.6f, "%.2f rad");
    }
    if (changed) m_vehicleDirty = true;

    ImGui::Separator();
    if (m_vehicleDirty) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Unsaved changes");
    if (ImGui::Button("Save vehicle")) {
        if (fixed()) saveVariant();
        saveVehicle();
    }
    ImGui::SameLine();
    const bool canTest = m_host.spawn && !v.wheels.empty();
    ImGui::BeginDisabled(!canTest);
    if (ImGui::Button("Save and test drive")) {
        if (fixed()) saveVariant();
        if (saveVehicle()) {
            if (m_host.selectForSpawnKey) m_host.selectForSpawnKey(pathFor(".fvehicle"));
            m_host.spawn(pathFor(".fvehicle"));
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Saves, spawns it in front of the Game camera and makes it the vehicle of the B key.\nIn the Game view: click, walk to it, press F to get in.");
    if (!m_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m_error.c_str());
}

// ---- viewport --------------------------------------------------------------------------------------------------------

void AssetForge::drawVehicleOverlay(const View& v, bool hovered) {
    const VehicleDef& d = m_vehicle;
    f32 minZ = FLT_MAX, maxZ = -FLT_MAX;
    for (const WheelDef& w : d.wheels) {
        minZ = std::min(minZ, w.position.z);
        maxZ = std::max(maxZ, w.position.z);
    }
    const f32 tolerance = 0.1f * (maxZ - minZ);
    for (size_t i = 0; i < d.wheels.size(); ++i) {
        const WheelDef& w = d.wheels[i];
        const bool selected = static_cast<int>(i) == m_selectedWheel && m_vehicleHandle == VehicleHandle::Wheel;
        const ImU32 color = selected ? IM_COL32(255, 230, 90, 255) : kAxleColor[w.position.z >= maxZ - tolerance ? 0 : 1];
        const f32 half = 0.5f * w.width;
        for (const f32 side : {-half, half}) circle3(v, w.position + Vec3(side, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1), w.radius, color, selected ? 2.5f : 1.5f);
        line3(v, w.position + Vec3(-half, 0, 0), w.position + Vec3(half, 0, 0), color, 1.5f);
        // Suspension travel.
        const Vec3 foot = w.position + Vec3(0, 0, 0);
        line3(v, foot + Vec3(0, -d.travelDown, 0), foot + Vec3(0, d.travelUp, 0), IM_COL32(255, 120, 80, 255), 3.0f);
        if (w.steered) line3(v, w.position + Vec3(0, w.radius, 0), w.position + Vec3(0, w.radius, 0.35f), IM_COL32(120, 255, 120, 255), 2.0f);
        if (w.driven) label3(v, w.position + Vec3(0, w.radius, 0), "drive", color);
    }
    // Collider and centre of mass.
    box3(v, d.colliderCenter, d.colliderHalf, m_vehicleHandle == VehicleHandle::ColliderCenter ? IM_COL32(255, 230, 90, 255) : IM_COL32(255, 200, 60, 140), 1.5f);
    const Vec3 com = d.centerOfMass;
    const ImU32 comColor = IM_COL32(255, 140, 40, 255);
    line3(v, com - Vec3(0.25f, 0, 0), com + Vec3(0.25f, 0, 0), comColor, 2.5f);
    line3(v, com - Vec3(0, 0.25f, 0), com + Vec3(0, 0.25f, 0), comColor, 2.5f);
    line3(v, com - Vec3(0, 0, 0.25f), com + Vec3(0, 0, 0.25f), comColor, 2.5f);
    label3(v, com, "center of mass", comColor);
    // Seat and exit.
    label3(v, d.driverSeat, "driver", IM_COL32(120, 220, 255, 255));
    box3(v, d.driverSeat, Vec3(0.12f, 0.18f, 0.12f), IM_COL32(120, 220, 255, 255), 2.0f);
    const Vec3 exitPoint(d.driverSeat.x * 0.0f + d.exitOffset.x, d.exitOffset.y + 0.05f, d.exitOffset.z);
    line3(v, d.driverSeat, exitPoint + Vec3(0, 0.9f, 0), IM_COL32(120, 220, 255, 90), 1.0f);
    circle3(v, exitPoint + Vec3(0, 0.02f, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), 0.3f, IM_COL32(120, 220, 255, 255), 2.0f);
    label3(v, exitPoint, "exit", IM_COL32(120, 220, 255, 255));
    // The front.
    const Vec3 nose(0.0f, 0.05f, d.colliderCenter.z + d.colliderHalf.z + 0.1f);
    line3(v, nose, nose + Vec3(0, 0, 0.8f), IM_COL32(90, 255, 120, 255), 3.0f);
    line3(v, nose + Vec3(0, 0, 0.8f), nose + Vec3(0.25f, 0, 0.45f), IM_COL32(90, 255, 120, 255), 3.0f);
    line3(v, nose + Vec3(0, 0, 0.8f), nose + Vec3(-0.25f, 0, 0.45f), IM_COL32(90, 255, 120, 255), 3.0f);
    label3(v, nose + Vec3(0, 0, 0.9f), "FRONT (+Z)", IM_COL32(90, 255, 120, 255));

    // The handle being moved.
    Vec3* target = nullptr;
    switch (m_vehicleHandle) {
    case VehicleHandle::Wheel:
        if (!d.wheels.empty()) target = &m_vehicle.wheels[static_cast<size_t>(std::clamp(m_selectedWheel, 0, static_cast<int>(d.wheels.size()) - 1))].position;
        break;
    case VehicleHandle::CenterOfMass: target = &m_vehicle.centerOfMass; break;
    case VehicleHandle::ColliderCenter: target = &m_vehicle.colliderCenter; break;
    case VehicleHandle::DriverSeat: target = &m_vehicle.driverSeat; break;
    case VehicleHandle::Exit: target = &m_vehicle.exitOffset; break;
    }
    if (target && moveHandle(v, 1, *target, hovered)) {
        if (m_vehicleHandle == VehicleHandle::Wheel && m_mirrorWheels) mirrorWheel(m_selectedWheel);
        m_vehicleDirty = true;
    }
}

} // namespace foh
