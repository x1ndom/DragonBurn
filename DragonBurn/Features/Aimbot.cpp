#include "Aimbot.h"
#include "../Helpers/Mouse.h"
#undef max()
#undef min()

void AimControl::switchToggle()
{
    LegitBotConfig::AimAlways = !LegitBotConfig::AimAlways;
}

std::pair<float, float> AimControl::CalculateTargetOffset(const Vec2& ScreenPos, int ScreenCenterX, int ScreenCenterY)
{
    float TargetX = 0.0f;
    float TargetY = 0.0f;

    /*x*/
    if (ScreenPos.x != ScreenCenterX) {
        TargetX = (ScreenPos.x > ScreenCenterX) ?
            -(ScreenCenterX - ScreenPos.x) :
            ScreenPos.x - ScreenCenterX;

        if (TargetX + ScreenCenterX > ScreenCenterX * 2 || TargetX + ScreenCenterX < 0) {
            TargetX = 0.0f;
        }
    }

    /*y*/
    if (ScreenPos.y != 0 && ScreenPos.y != ScreenCenterY) {
        TargetY = (ScreenPos.y > ScreenCenterY) ?
            -(ScreenCenterY - ScreenPos.y) :
            ScreenPos.y - ScreenCenterY;

        if (TargetY + ScreenCenterY > ScreenCenterY * 2 || TargetY + ScreenCenterY < 0) {
            TargetY = 0.0f;
        }
    }

    return { TargetX, TargetY };
}

// New Humanize function based on the user's document
std::pair<float, float> AimControl::Humanize(float TargetX, float TargetY, float Norm) {
    if (!HumanizeVar) {
        return { TargetX, TargetY };
    }

    // --- Dynamic Smoothing ---
    // The closer the crosshair is to the target, the more smoothing is applied.
    // This creates a more natural deceleration.
    float DistRatio = Norm / AimFov; // How far we are from the target, in a [0, 1] range.
    float SmoothingFactor = Smooth * (1.0f - DistRatio); // More smoothing closer to target.
    SmoothingFactor = std::clamp(SmoothingFactor, 0.f, 10.f); // Clamp to avoid excessive smoothing.

    float SmoothedX = TargetX / (SmoothingFactor > 1.f ? SmoothingFactor : 1.f);
    float SmoothedY = TargetY / (SmoothingFactor > 1.f ? SmoothingFactor : 1.f);

    // --- Expanded Random Jitter ---
    // Adds small, random inaccuracies to simulate human hand tremor.
    if (Norm > 1.0f) { // Only apply jitter for significant movements
        std::uniform_real_distribution<float> jitter_dist(-Norm / 15.0f, Norm / 15.0f); // Jitter scales with distance
        SmoothedX += jitter_dist(gen);
        SmoothedY += jitter_dist(gen);
    }
    
    // --- Variable Delay ---
    // Adds a tiny, random delay to each mouse movement to break the robotic consistency.
    std::uniform_int_distribution<> delay_dist(10, 35); // 10ms to 35ms random delay
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_dist(gen)));

    // Update previous target, but with some interpolation to prevent jerky stops
    PrevTargetX = (TargetX + PrevTargetX) / 2.0f;
    PrevTargetY = (TargetY + PrevTargetY) / 2.0f;
    
    return { SmoothedX, SmoothedY };
}

void AimControl::ApplyRCS(Vec3& OppPos, const CEntity& Local)
{
    if (!LegitBotConfig::RCS || Local.Pawn.ShotsFired <= 1)
        return;

    // Humanized RCS logic based on the document
    std::uniform_real_distribution<float> rcs_factor(0.7f, 0.95f); // 70-95% compensation
    std::uniform_real_distribution<float> jitter_dist(-0.1f, 0.1f); // Small random jitter

    Vec2 aimPunch = Local.Pawn.AimPunchAngle;

    // Apply humanized factor
    aimPunch.x *= rcs_factor(gen);
    aimPunch.y *= rcs_factor(gen);

    // Apply jitter
    aimPunch.x += jitter_dist(gen);
    aimPunch.y += jitter_dist(gen);

    // Convert punch angles to a vector and apply it
    float RcsX = cos(Local.Pawn.ViewAngle.y * M_PI / 180.f) * (aimPunch.x * 2.f) - sin(Local.Pawn.ViewAngle.y * M_PI / 180.f) * (aimPunch.y * 2.f);
    float RcsY = sin(Local.Pawn.ViewAngle.y * M_PI / 180.f) * (aimPunch.x * 2.f) + cos(Local.Pawn.ViewAngle.y * M_PI / 180.f) * (aimPunch.y * 2.f);
    float RcsZ = aimPunch.x * 2.f; // This is a simplification, but works for vertical recoil

    OppPos.x -= RcsY;
    OppPos.y += RcsX;
    OppPos.z -= RcsZ;
}

void AimControl::AimBot(const CEntity& Local, Vec3 LocalPos, std::vector<std::pair<Vec3, int>>& AimPosList)
{
    if (MenuConfig::ShowMenu)
        return;

    // Basic checks
    std::string curWeapon = TriggerBot::GetWeapon(Local);
    if (!TriggerBot::CheckWeapon(curWeapon) || (onlyAuto && !CheckAutoMode(curWeapon)) || (Local.Pawn.ShotsFired <= AimBullet - 1 && AimBullet != 0)) {
        wasAimingLastFrame = false; HasTarget = false;
        return;
    }
    if (AimControl::ScopeOnly) {
        bool isScoped;
        memoryManager.ReadMemory<bool>(Local.Pawn.Address + Offset.Pawn.isScoped, isScoped);
        if (!isScoped && TriggerBot::CheckScopeWeapon(curWeapon)) {
            wasAimingLastFrame = false; HasTarget = false;
            return;
        }
    }
    if (!IgnoreFlash && Local.Pawn.FlashDuration > 0.f) {
        wasAimingLastFrame = false; HasTarget = false;
        return;
    }

    // Weighted Bone Selection
    std::vector<std::pair<Vec3, int>> weightedAimPosList;
    for (const auto& aimPos : AimPosList) {
        int boneID = aimPos.second;
        int weight = 0;
        switch (boneID) {
        case BONEINDEX::head: weight = 6; break; // 60%
        case BONEINDEX::neck_0: weight = 2; break; // 20%
        case BONEINDEX::spine_2: weight = 2; break; // 20% (chest)
        default: weight = 1; break;
        }
        for (int i = 0; i < weight; ++i) {
            weightedAimPosList.push_back(aimPos);
        }
    }

    if (weightedAimPosList.empty()) {
        wasAimingLastFrame = false; HasTarget = false;
        return;
    }

    // Select a random target from the weighted list
    std::uniform_int_distribution<> dist(0, weightedAimPosList.size() - 1);
    std::pair<Vec3, int> selectedTarget = weightedAimPosList[dist(gen)];
    Vec3 finalAimPos = selectedTarget.first;

    // Calculate angle to the selected target
    Vec3 OppPos = finalAimPos - LocalPos;
    ApplyRCS(OppPos, Local); // Apply humanized RCS
    const float Distance = sqrt(OppPos.x * OppPos.x + OppPos.y * OppPos.y);
    float Yaw = atan2f(OppPos.y, OppPos.x) * 57.295779513f - Local.Pawn.ViewAngle.y;
    float Pitch = -atan(OppPos.z / Distance) * 57.295779513f - Local.Pawn.ViewAngle.x;
    float Norm = sqrt(Yaw * Yaw + Pitch * Pitch);

    if (Norm >= AimFov || Norm <= AimFovMin) {
        wasAimingLastFrame = false; HasTarget = false;
        return;
    }

    // Random Reaction Time
    if (!wasAimingLastFrame) {
        std::uniform_int_distribution<> reaction_dist(50, 200); // 50-200ms
        std::this_thread::sleep_for(std::chrono::milliseconds(reaction_dist(gen)));
    }
    wasAimingLastFrame = true;

    // Convert to screen coordinates and apply humanization
    Vec2 ScreenPos;
    if (!gGame.View.WorldToScreen(finalAimPos, ScreenPos)) {
        HasTarget = false;
        return;
    }

    HasTarget = true;
    const int ScreenCenterX = Gui.Window.Size.x / 2;
    const int ScreenCenterY = Gui.Window.Size.y / 2;
    auto [TargetX, TargetY] = CalculateTargetOffset(ScreenPos, ScreenCenterX, ScreenCenterY);

    TargetX /= Local.Client.Sensitivity / 4;
    TargetY /= Local.Client.Sensitivity / 4;

    if (HumanizeVar) {
        auto [HumanizedX, HumanizedY] = Humanize(TargetX, TargetY, Norm);
        TargetX = HumanizedX;
        TargetY = HumanizedY;
    } else if (Smooth > 0.0f) {
        TargetX /= Smooth;
        TargetY /= Smooth;
    }

    // Move mouse
    static DWORD lastAimTime = GetTickCount64();
    DWORD currentTick = GetTickCount64();
    if (currentTick - lastAimTime >= MenuConfig::AimDelay) {
        mouse_move(0, static_cast<char>(TargetX), static_cast<char>(TargetY), 0);
        lastAimTime = currentTick;
    }
}

bool AimControl::CheckAutoMode(const std::string& WeaponName)
{
    if (WeaponName == "deagle" || WeaponName == "elite" || WeaponName == "fiveseven" || WeaponName == "glock" || WeaponName == "awp" || WeaponName == "xm1014" || WeaponName == "mag7" || WeaponName == "sawedoff" || WeaponName == "tec9" || WeaponName == "zeus" || WeaponName == "p2000" || WeaponName == "nova" || WeaponName == "p250" || WeaponName == "ssg08" || WeaponName == "usp" || WeaponName == "revolver")
        return false;
    else
        return true;
}