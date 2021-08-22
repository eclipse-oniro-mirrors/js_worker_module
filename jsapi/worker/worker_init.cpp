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

#include "worker_init.h"

namespace OHOS::CCRuntime::Worker {
InitWorkerFunc WorkerCore::initWorkerFunc = NULL;
GetAssetFunc WorkerCore::getAssertFunc = NULL;
OffWorkerFunc WorkerCore::offWorkerFunc = NULL;

void WorkerCore::RegisterInitWorkerFunc(InitWorkerFunc func)
{
    if (func != nullptr) {
        WorkerCore::initWorkerFunc = func;
    }
}

void WorkerCore::RegisterAssetFunc(GetAssetFunc func)
{
    if (func != nullptr) {
        WorkerCore::getAssertFunc = func;
    }
}

void WorkerCore::RegisterOffWorkerFunc(OffWorkerFunc func)
{
    if (func != nullptr) {
        WorkerCore::offWorkerFunc = func;
    }
}
}  // namespace OHOS::CCRuntime::Worker