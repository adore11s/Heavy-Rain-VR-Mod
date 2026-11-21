#pragma once
// 0.33.0 (playbook 19.8 point 11 Gate 3 row 9): the engine's own link between the player and an object a scene clip
// moves - the playing MOVIE that plays both (docs/items/HOLDING.md route 1 / 4, TARGET_ADAPTER_MAP "Movies").
//
// STATIC 2026-11-20: MOVIE_CONTROLLER_PLAY::Update 0x1403c74e0 runs per frame for each playing movie controller
// (controller +0x110 = the movie's COM instance; the movie plays while MOVIE +0x2b0 bit 0 is set). MOVIE +0x150 is an
// array (+0x158 count) of pointers to 0x18-byte records whose +0 is the entity's COM instance (MOVIE reader
// 0x1403aea10). A COM instance carries its name at +0x18 (engine string, length +0x20 - RBS_OPERATOR_CI_GET_NAME
// 0x14055e630) and its object at +0x50 when cached (flags +0x30 & 0x202000). Script entity +0x160 -> its 3D entity
// -> +0x68 its scene NODE (bones::CharacterNode). The names are the ones Gate 1 / 2 key every pair by, so the class
// table (item_class.h) applies at run time. INFERENCE until a log: the run-time instance names equal the data's names
// (`FPITEM movie ...` lines list them, with the table's answer for each).
//
// Read-only: nothing in the game is written. Game thread (the update); fpbody reads the snapshot in the draw.
namespace hrvr::movielink {

bool Install();
bool Available();

struct Link {
    int cls = -1;        // itemclass::Class, or -1 = not in a playing movie with the player
    char entity[64] = {};
    char movie[64] = {};
};

// Is `node` (or its parent node `parent`, may be null) an entity of a movie that plays with the player's node
// `player` right now (seen within the last 250 ms)? Fills `out` when it is.
bool Find(void* node, const void* parent, void* player, Link* out);

// 1.0.5 (playbook 19.8 point 11 L1 census): every object of the movies that play with the player's node right now
// (the player itself left out), up to `max`. Returns the count written.
struct LinkedObject {
    void* node = nullptr;
    int cls = -1;
    char entity[64] = {};
};
int ListLinked(void* player, LinkedObject* out, int max);

// "fpstatus": the movies seen in the last second.
void LogStatus();

}  // namespace hrvr::movielink
