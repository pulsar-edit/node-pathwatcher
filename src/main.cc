#include "common.h"
#include "addon-data.h"

namespace {

  Napi::Object Init(Napi::Env env, Napi::Object exports) {
    auto* data = new AddonData(env);
    env.SetInstanceData(data);

    CommonInit(env);
    PlatformInit(env);

    exports.Set("setCallback", Napi::Function::New(env, SetCallback));
    exports.Set("watch", Napi::Function::New(env, Watch));
    exports.Set("unwatch", Napi::Function::New(env, Unwatch));

    return exports;
  }

} // namespace

NODE_API_MODULE(pathwatcher, Init)
