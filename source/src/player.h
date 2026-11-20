#pragma once
// The character the player controls, and whether the player is walking it (B2S guide 2.2, [HR-NEW]).
//
// STATIC (2026-10-27): THIRD_PERSON_GAMEPLAY (TPG, one per playable character set-up) is the gameplay
// that walks a character. Its script properties (combasis_140214920, offsets from a null base):
// "Character" handle at +0x1E8 (class 0xfa6 = CHARACTER), "CtrlMode" at +0x200 (ORIENTATION_CONVERSION_MODE:
// 0 CAMERA_RELATIVE, 1 ROOT_CHARACTER_RELATIVE, 2 CHARACTER_RELATIVE), and the script getter IsActive reads
// the byte at +0x54 (combasis_140217390). Active TPG -> the player walks its character.
// Without an active TPG (cutscenes, QTE sequences) the character is the camera target alias named
// "player" (CAMERA_TARGET_ALIAS: +0x18 name, +0x30 entity handle; CAMERA_MODIFIER_HEAD's default target
// is "player.Head"), else the last walked character while it is alive.
namespace hrvr::player {

enum class Control { Unknown, Walking, Cutscene };

struct State {
    void* character = nullptr;  // SINT::CHARACTER, nullptr when unknown
    void* tpg = nullptr;        // the active THIRD_PERSON_GAMEPLAY while walking
    int ctrlMode = 0;           // TPG CtrlMode while walking
    Control control = Control::Unknown;
};

// Render or game thread, once per frame. Starts a background heap scan (rate-limited) when needed.
// 0.36.8: with no walked character and no "player" alias, the chapter's main character is chosen (player.cpp: the one
// walked here, the chapter's saved choice, the last one played, else random - saved per chapter in hrvr.ini).
State Poll();
// 0.36.8: the game camera's position this frame (fpcam), so a new choice prefers main characters within 20 m of it.
void SetCamera(float x, float y, float z);
const char* Name(Control c);
// 0.20.0: the characters walking as a pair with `character` (SCRIPT_ENTITY_CONTROLLER_MOKIT_SYNC: one's motion kit
// played in step with the other's; Meet Madison at the motel, Fugitive Lizard / Subway). Returns how many.
int Partners(void* character, void** out, int max);
// Logs every known TPG and the aliases (diagnostic command "fpplayer").
void Dump();

}  // namespace hrvr::player
