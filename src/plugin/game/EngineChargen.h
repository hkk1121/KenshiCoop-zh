#ifndef KENSHICOOP_ENGINE_CHARGEN_H
#define KENSHICOOP_ENGINE_CHARGEN_H
#include "../../netproto/CharacterCreation.h"

class GameWorld;
class Character;
namespace coop { namespace engine {
struct CharacterChoice {
    std::string startSid, label;
};
// 捏人确认后读出的一个开局角色的外观（句柄 + 草稿）
struct StartAppearance {
    unsigned int hand[5];
    CharacterDraft draft;
};
bool characterCreationHooks();
void characterChoices(GameWorld* gw, std::vector<CharacterChoice>& out);
// 主机：按原版顺序选开局出生点（指定坐标 → 开局城镇里随机一个）；都没有返回 false
bool startPosition(GameWorld* gw, const std::string& startSid, float out[3]);
// 主机：生成开局全部编队并完成开局科技；返回新编队标签、第一个角色的句柄和开局资金
bool createStartSquads(GameWorld* gw, const std::string& startSid, const float pos[3],
                       std::vector<std::pair<unsigned int, unsigned int> >& tabs, unsigned int first[5], int* money);
// 申请人：把这些编队里可捏的角色交给原版捏人界面
bool beginStartEditing(GameWorld* gw, const std::vector<std::pair<unsigned int, unsigned int> >& tabs, const std::string& startSid);
int pollCharacterCreation(GameWorld* gw, std::vector<StartAppearance>& out);
bool cancelCharacterCreation(bool worldAlive);
bool characterEditing();
unsigned int playerTabSize(GameWorld* gw, unsigned int container, unsigned int serial);
bool applyCharacterDraft(GameWorld* gw, Character* c, const CharacterDraft& draft);
} }
#endif
