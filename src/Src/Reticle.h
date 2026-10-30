#pragma once

//! A reticle per hand: where each controller is pointing, drawn into the frame the headset receives.
//!
//! It is screen-space on purpose. The frame the mod submits to OpenXR is the game's own backbuffer
//! (Overlay::OnPresent runs BEFORE VRRuntime::OnPresent, so anything drawn there reaches the headset), and the
//! projection the world was rendered with is recovered exactly by GpuTrace's MVP factorisation -- so projecting a
//! world point onto that image needs no FOV, aspect or letterbox calibration, and no new shader or vertex buffer
//! that could disturb the game's own state. A 3D marker would need all of that to end up in the same place.
//!
//! What it shows, per hand: a line from the hand along the controller's forward, and a ring at the end of it. The
//! ring sits on the engine's own landing point when the engine has computed one (the power's target while the
//! trigger is held, or the last shot's aim point), and otherwise at a fixed range -- so the line always answers
//! "where am I pointing" and the ring answers "where will it land" whenever the game knows.
namespace Reticle
{
    //! 0 = off, 1 = both hands, 2 = the power hand only, 3 = the gun hand only.
    void SetMode(int mode);
    //! A small red aiming point for the left controller, independently of the debug rays/rings.
    void SetAimDot(bool on);
    bool AimDot();
    void AddMark(const float* world);            //!< research: a world point drawn as a cross (`vr reticlemark x y z`)
    void ClearMarks();
    int  Mode();
    //! The palm/controller axes overlay: the instrument for judging a hand pose from a screenshot.
    void SetAxes(int on);
    int  Axes();
    void SetRange(float metres);        //!< how far along the ray the ring sits with no engine target (default 8 m)
    float Range();
    //! Draw into the current ImGui frame's foreground list. Called from Overlay::OnPresent, which owns the frame.
    void Draw();
    struct Stats { unsigned long long frames = 0, drawn = 0; int handValid = 0, projValid = 0, engineTarget = 0; };
    Stats GetStats();
}

