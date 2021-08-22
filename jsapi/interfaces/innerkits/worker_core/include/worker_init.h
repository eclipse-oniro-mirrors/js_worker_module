/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FOUNDATION_CCRUNTIME_JSAPI_INTERFACES_INNERKITS_WORKER_CORE_INCLUDE_H
#define FOUNDATION_CCRUNTIME_JSAPI_INTERFACES_INNERKITS_WORKER_CORE_INCLUDE_H

#include <functional>

#include "native_engine/native_engine.h"

using InitWorkerFunc = std::function<void(NativeEngine*)>;
using GetAssetFunc = std::function<void(const std::string& uri, std::vector<uint8_t>&)>;
using OffWorkerFunc = std::function<void(NativeEngine*)>;

namespace OHOS::CCRuntime::Worker {
class WorkerCore {
public:
    static InitWorkerFunc initWorkerFunc;
    static void RegisterInitWorkerFunc(InitWorkerFunc func);

    static GetAssetFunc getAssertFunc;
    static void RegisterAssetFunc(GetAssetFunc func);

    static OffWorkerFunc offWorkerFunc;
    static void RegisterOffWorkerFunc(OffWorkerFunc func);
};
} // namespace OHOS::CCRuntime::Worker
#endif // FOUNDATION_CCRUNTIME_JSAPI_INTERFACES_INNERKITS_WORKER_CORE_INCLUDE_H
