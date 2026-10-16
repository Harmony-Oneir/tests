#include <cstdio>

#include "ipc_skeleton.h"
#include "iservice_registry.h"
#include "message_option.h"
#include "message_parcel.h"

using namespace OHOS;

constexpr int32_t TEST_SA_ID = 9000;

int main()
{
    auto sam = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam == nullptr) {
        printf("samgr not reachable\n");
        return 1;
    }

    auto remote = sam->GetSystemAbility(TEST_SA_ID);
    if (remote == nullptr) {
        printf("SA %d not found\n", TEST_SA_ID);
        return 1;
    }

    MessageParcel data;
    MessageParcel reply;
    MessageOption option;
    data.WriteString("ping");

    int ret = remote->SendRequest(1, data, reply, option);
    printf("client: ret=%d reply=%s\n", ret, reply.ReadString().c_str());
    return ret;
}
