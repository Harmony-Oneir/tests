#include <cstdio>

#include "ipc_object_stub.h"
#include "ipc_skeleton.h"
#include "iservice_registry.h"
#include "message_option.h"
#include "message_parcel.h"

using namespace OHOS;

constexpr int32_t TEST_SA_ID = 9000;

class TestStub : public IPCObjectStub {
public:
    TestStub() : IPCObjectStub(u"arkvm.test") {}

    int OnRemoteRequest(uint32_t code, MessageParcel &data, MessageParcel &reply,
                        MessageOption &option) override
    {
        std::string payload = data.ReadString();
        printf("server: code=%u payload=%s\n", code, payload.c_str());
        reply.WriteString("pong");
        return 0;
    }
};

int main()
{
    sptr<IRemoteObject> stub = new TestStub();

    auto sam = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam == nullptr) {
        printf("samgr not reachable\n");
        return 1;
    }

    int32_t ret = sam->AddSystemAbility(TEST_SA_ID, stub);
    printf("AddSystemAbility ret=%d\n", ret);

    IPCSkeleton::JoinWorkThread();
    return 0;
}
