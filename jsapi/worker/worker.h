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

#ifndef FOUNDATION_CCRUNTIME_JSAPI_WORKER_H
#define FOUNDATION_CCRUNTIME_JSAPI_WORKER_H

#include <list>
#include <map>
#include <mutex>
#include <uv.h>
#include "message_queue.h"
#include "napi/native_node_api.h"
#include "native_engine/native_engine.h"
#include "utils/log.h"
#include "worker_helper.h"
#include "worker_runner.h"

namespace OHOS::CCRuntime::Worker {
class Worker {
public:
    static const int8_t WORKERPARAMNUM = 2;

    enum RunnerState { STARTING, RUNNING, TERMINATEING, TERMINATED };

    enum ListenerMode { ONCE, PERMANENT };

    enum ScriptMode { CLASSIC, MODULE };

    class WorkerListener {
    public:
        WorkerListener() : callback_(nullptr), worker_(nullptr), mode_(PERMANENT) {}

        explicit WorkerListener(Worker* worker) : callback_(nullptr), worker_(worker), mode_(PERMANENT) {}

        WorkerListener(Worker* worker, ListenerMode mode) : callback_(nullptr), worker_(worker), mode_(mode) {}

        ~WorkerListener()
        {
            callback_ = nullptr;
            worker_ = nullptr;
        }

        bool NextIsAvailable() const
        {
            return mode_ != ONCE;
        }

        ListenerMode GetListenerMode() const
        {
            return mode_;
        }

        napi_ref GetCallback() const
        {
            return callback_;
        }

        void SetCallable(napi_env env, napi_value value)
        {
            napi_create_reference(env, value, 1, &callback_);
        }

        void SetMode(ListenerMode mode)
        {
            mode_ = mode;
        }

        bool operator==(const WorkerListener& listener) const;

    private:
        napi_ref callback_ {NULL};
        Worker* worker_ {nullptr};
        ListenerMode mode_ {PERMANENT};
    };

    struct FindWorkerListener {
        FindWorkerListener(napi_env env, napi_ref ref) : env_(env), ref_(ref) {}

        bool operator()(const WorkerListener* listener) const
        {
            napi_ref compareRef = listener->GetCallback();
            napi_value compareObj = nullptr;
            napi_get_reference_value(env_, compareRef, &compareObj);

            napi_value obj = nullptr;
            napi_get_reference_value(env_, ref_, &obj);
            bool isEqual = false;
            napi_strict_equals(env_, compareObj, obj, &isEqual);
            return isEqual;
        }

        napi_env env_;
        napi_ref ref_;
    };
    using FindWorkerListener = struct FindWorkerListener;

    Worker(NativeEngine* env, napi_ref thisVar);
    ~Worker();

    static void MainOnMessage(const uv_async_t* req);
    static void MainOnError(const uv_async_t* req);
    static void WorkerOnMessage(const uv_async_t* req);
    static void ExecuteInThread(const void* data);
    static void PrepareForWorkerInstance(const Worker* worker);

    static napi_value PostMessage(napi_env env, napi_callback_info cbinfo);
    static napi_value PostMessageToMain(napi_env env, napi_callback_info cbinfo);
    static napi_value Terminate(napi_env env, napi_callback_info cbinfo);
    static napi_value CloseWorker(napi_env env, napi_callback_info cbinfo);
    static napi_value On(napi_env env, napi_callback_info cbinfo);
    static napi_value Once(napi_env env, napi_callback_info cbinfo);
    static napi_value Off(napi_env env, napi_callback_info cbinfo);
    static napi_value AddEventListener(napi_env env, napi_callback_info cbinfo);
    static napi_value DispatchEvent(napi_env env, napi_callback_info cbinfo);
    static napi_value RemoveEventListener(napi_env env, napi_callback_info cbinfo);
    static napi_value RemoveAllListener(napi_env env, napi_callback_info cbinfo);

    static napi_value AddListener(napi_env env, napi_callback_info cbinfo, ListenerMode mode);
    static napi_value RemoveListener(napi_env env, napi_callback_info cbinfo);

    static napi_value WorkerConstructor(napi_env env, napi_callback_info cbinfo);
    static napi_value InitWorker(napi_env env, napi_value exports);

    void StartExecuteInThread(napi_env env, const char* script);

    bool UpdateWorkerState(RunnerState runnerState);

    bool IsRunning() const
    {
        return runnerState_.load(std::memory_order_acquire) == RUNNING;
    }

    bool IsTerminated() const
    {
        return runnerState_.load(std::memory_order_acquire) >= TERMINATED;
    }

    bool IsTerminating() const
    {
        return runnerState_.load(std::memory_order_acquire) == TERMINATEING;
    }

    void SetScriptMode(ScriptMode mode)
    {
        scriptMode_ = mode;
    }

    void AddListenerInner(napi_env env, const char* type, const WorkerListener* listener);
    void RemoveListenerInner(napi_env env, const char* type, napi_ref callback);
    void RemoveAllListenerInner();

    uv_loop_t* GetWorkerLoop() const
    {
        if (workerEngine_ != nullptr) {
            return workerEngine_->GetUVLoop();
        }
        return nullptr;
    }

    const NativeEngine* GetWorkerEngine() const
    {
        return workerEngine_;
    }

    void SetWorkerEngine(NativeEngine* engine)
    {
        workerEngine_ = engine;
    }

    const NativeEngine* GetMainEngine() const
    {
        return mainEngine_;
    }

    const char* GetScript() const
    {
        return script_;
    }

    const char* GetName() const
    {
        return name_;
    }

    uv_loop_t* GetMainLoop() const
    {
        if (mainEngine_ != nullptr) {
            return mainEngine_->GetUVLoop();
        }
        return nullptr;
    }

private:
    void WorkerOnMessageInner(const NativeEngine* engine);
    void MainOnMessageInner(const NativeEngine* engine);
    void MainOnErrorInner(const NativeEngine* engine);
    void MainOnMessageErrorInner(const NativeEngine* engine);
    void WorkerOnMessageErrorInner(const NativeEngine* engine);
    void WorkerOnErrorInner(const NativeEngine* engine, napi_value error);

    void HandleException(const NativeEngine* engine);
    bool CallWorkerFunction(const NativeEngine* engine, int argc, const napi_value* argv,
                            const char* methodName, bool tryCatch);
    void CallMainFunction(const NativeEngine* engine, int argc, const napi_value* argv, const char* methodName) const;

    void HandleEventListeners(napi_env env, napi_value recv, size_t argc, const napi_value* argv, const char* type);
    void TerminateInner();

    void PostMessageInner(MessageDataType data);
    void PostMessageToMainInner(MessageDataType data);

    void TerminateWorker();
    void CloseInner();

    void PublishWorkerOverSignal();
    void CloseWorkerCallback();
    void CloseMainCallback() const;

    char* script_ {nullptr};
    char* name_ {nullptr};
    ScriptMode scriptMode_ {CLASSIC};

    MessageQueue workerMessageQueue_;
    MessageQueue mainMessageQueue_;
    MessageQueue errorQueue_;

    uv_async_t workerOnMessageSignal_;
    uv_async_t mainOnMessageSignal_;
    uv_async_t mainOnErrorSignal_;

    std::atomic<RunnerState> runnerState_;
    WorkerRunner* runner_ {nullptr};

    NativeEngine* mainEngine_ {nullptr};
    NativeEngine* workerEngine_ {nullptr};

    napi_ref workerWrapper_ {nullptr};
    napi_ref parentPort_ {nullptr};

    std::map<std::string, std::list<WorkerListener*>> eventListeners_;

    std::mutex workerAsyncMutex_;

    friend class WorkerListener;
};
} // namespace OHOS::CCRuntime::Worker
#endif // FOUNDATION_CCRUNTIME_JSAPI_WORKER_H