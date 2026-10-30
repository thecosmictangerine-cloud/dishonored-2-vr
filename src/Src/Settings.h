#pragma once
#include <string>
#include <vector>

//! The player's settings: <game>\DishonoredVR.ini, read once at startup. Every key has a default, so a missing file,
//! a missing key or a value that cannot be read all leave the mod working (and the log says which). Settings are
//! turned into the same `vr ...` commands the test channel uses, so a setting does exactly what its command does.
namespace Settings
{
    void Load();                                  //!< InitThread, before anything asks for a setting
    std::vector<std::string> StartupCommands();   //!< run at the first Present (input layout, turning)
    std::vector<std::string> PackageCommands();   //!< run when the VR package switches on in a game
    bool StatusFile();                            //!< write DVR_heartbeat.txt once a second (diagnostics)
    bool LogInput();                              //!< log every controller button change (diagnostics)
    bool Haptics();                               //!< controller vibration
    bool StereoHud();                             //!< put the game's 2D HUD on a finite-depth quad in stereo
    float StereoHudWidth();                       //!< apparent width of the stereo HUD quad, degrees (35 - 90)
    float RenderScale();                          //!< the game's render size relative to the headset's pixel density (0 = the window's)
    float ControllerPitchOffset();                //!< degrees the controllers' pointing is tilted (+ = up)
    std::string Summary();                        //!< one line for the log
}

