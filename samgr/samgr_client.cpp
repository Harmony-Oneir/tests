#include <cstdio>
#include "iservice_registry.h"
#include "string_ex.h"

int main()
{
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam == nullptr) {
        printf("samgr not reachable\n");
        return 1;
    }
    auto list = sam->ListSystemAbilities();
    printf("samgr ok, abilities: %zu\n", list.size());
    for (const auto& s : list) {
        printf("  %s\n", OHOS::Str16ToStr8(s).c_str());
    }
    return 0;
}
