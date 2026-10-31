#pragma once
#include <cstdint>
#include <map>
#include <string>

// the port's own settings, in the settings file through app::kGame.options
namespace game {

struct Options {
    bool fps_60 = true;       // the game's own 30 fps cap lifted: its logic counts vertical blanks, so it keeps its speed
    bool skip_intros = true;  // the EA and Free Radical logo movies at boot
    bool skip_any_cutscene = true;  // Start or A skips the cutscenes the game would not let go before they were seen
    // a loading screen ends when the load does, not when its bar, paced for the console's disc, has filled
    bool fast_loading = true;
    // in Story co-op each player's profile name over their head, as the team games' tags (game/name_tags.cpp)
    bool player_names = true;
    // the view's vertical angle in degrees on foot, the game's own 55; the held weapon's, 45 at that view, kept
    // whatever the view's (game/fov.cpp)
    uint32_t fov = 55;
    uint32_t weapon_fov = 45;
    // players 2 to 4's own, each on their part of a split screen; fov and weapon_fov are player 1's
    uint32_t player_fov[3] = {55, 55, 55};
    uint32_t player_weapon_fov[3] = {45, 45, 45};
    // the mouse turns the view itself: 100 is 0.1 degrees a count, scaled down by the zoom
    uint32_t mouse_sensitivity = 100;
    uint32_t mouse_vertical = 100;  // up and down at this percent of the sideways speed
    bool invert_mouse = false;
    // the right stick's aim (common/controls.cpp ShapeLook): a round dead zone, a percent of the stick's reach, in
    // place of the game's own, and the curve past it, a power in hundredths: 100 straight, over it finer near the
    // centre and as fast at the edge
    uint32_t stick_deadzone = 8;
    uint32_t stick_curve = 150;
    // player 1's controller's gyro turns the view as the mouse does, beside the right stick (common/controls.cpp
    // GyroAim): gyro_speed percent of the controller's own turn
    bool gyro_aim = false;
    uint32_t gyro_speed = 200;
    // the arms holding the weapon in the first person view (game/fov.cpp); off, the weapon alone, as in TimeSplitters 2
    bool hands = true;
    bool show_fps = false;
    // the performance overlay (panel.cpp DrawStats): where each frame's time goes, the CPU's threads and the GPU's work
    bool show_stats = false;
    // the same into the log (d3d12.cpp StatsLog): every ten seconds, each hitch, a summary every five minutes
    bool log_stats = false;
    // the rain falling around the view and running down the screen (game/weather.cpp)
    bool rain = true;
    // the blood (game/blood.cpp): the sprays from a hit and the splats and pools it leaves; in online co-op the host's
    bool blood = true;  // the frame rate in the top left corner (common/panel.cpp)
    // the keyboard and mouse are player 1 on a port of their own, the controllers on ports 2-4: a controller's player
    // is never the keyboard's. Off, the first controller shares port 1 with the keyboard.
    bool keyboard_own_port = true;
    // Discord Rich Presence (game/discord.cpp): what is played shown on the Discord profile, through the application
    // whose name Discord shows, this port's own
    bool discord = true;
    std::string discord_app_id = "1562387270034522133";
    // the picture beside it: an art asset's name in that application (Rich Presence > Art Assets) or an https link;
    // empty for none. The default is the cover (config/discord_cover.jpg) uploaded as "cover"
    std::string discord_image = "cover";
    // online co-op (net/lockstep.h, the settings panel's Co-op page): the host's address to join, the port hosted on
    // or joined, the session's password
    std::string net_address;
    uint32_t net_port = 7777;
    std::string net_password;
    // the text's font (game/ts2_style.cpp): 0 the game's Impact, 1 TimeSplitters 2's (from ts2_folder), 2 the early
    // subtitles' Arial Bold
    uint32_t font = 0;
    // the HUD (game/hud_style.cpp): 0 the game's, 1 the early Future Perfect's, after TimeSplitters 2's, 2 the game's
    // with TimeSplitters 1's health and armour bars and crosshair
    uint32_t hud_style = 0;
    // the player's own TimeSplitters 2 for Xbox: the folder of its default.xbe and data
    std::string ts2_folder;
    // the Cortez of Story (game/beta_cortez.cpp): 0 the game's, 1 the early Future Perfect's, from ts4_folder
    uint32_t cortez_model = 0;
    // the player's own leaked TimeSplitters 4 demo for GameCube, extracted: the folder of its assets and textures
    std::string ts4_folder;
    // the skyline of Breaking and Entering (game/beta_skyline.cpp): 0 the game's, 1 the early Future Perfect's, from
    // preview_path
    uint32_t skyline_model = 0;
    // Story's players' characters (game/story_characters.cpp): 0 each mission's own, else that Arcade character's,
    // its place in the Arcade list and 1
    uint32_t story_character[2] = {0, 0};
    // the player's own November 2004 preview of Future Perfect for PS2: its disc image (.iso) or a folder of its files
    std::string preview_path;
    // the mods switched off (common/mods.h): their folders' names, comma separated
    std::string mods_off;
    // [keys]: action name -> comma separated keys (controls.cpp kActions and kNames)
    std::map<std::string, std::string> keys;
};
Options& Opt();
// a player's angles by their slot, 0 to 3
uint32_t& Fov(uint32_t slot);
uint32_t& WeaponFov(uint32_t slot);
// in a System Link or online session the angles are the game's own, 55 and 45, for everyone alike (game/fov.cpp)
bool FovLocked();
bool FreeCamOn();  // the free camera flies (free_cam.cpp)

}  // namespace game
