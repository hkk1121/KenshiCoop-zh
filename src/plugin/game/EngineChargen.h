#ifndef KENSHICOOP_ENGINE_CHARGEN_H
#define KENSHICOOP_ENGINE_CHARGEN_H
#include "../../netproto/CharacterCreation.h"

class GameWorld;
class Character;
namespace coop { namespace engine {
struct CharacterChoice {
    std::string startSid, templateSid, label;
};
bool characterCreationHooks();
void characterChoices(GameWorld* gw, std::vector<CharacterChoice>& out);
bool beginCharacterCreation(GameWorld* gw, const CharacterChoice& choice);
int pollCharacterCreation(GameWorld* gw, CharacterDraft& out);
bool cancelCharacterCreation(bool worldAlive, bool resumeSave = true);
bool characterPreview(const Character* c = 0);
bool deferCharacterSave(const std::string* name, bool autosave);
unsigned int playerTabSize(GameWorld* gw, unsigned int container, unsigned int serial);
bool applyCharacterDraft(GameWorld* gw, Character* c, const CharacterDraft& draft);
bool createDraftCharacter(GameWorld* gw, const CharacterDraft& draft, const float* position, unsigned int out[5]);
} }
#endif
