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

#include "worker.h"

#include "worker_init.h"

namespace OHOS::CCRuntime::Worker {
const static int MAXWORKERS = 50;
static std::list<Worker*> g_workers;
static std::mutex g_workersMutex;

Worker::Worker(NativeEngine* env, napi_ref thisVar)
    : script_(nullptr), name_(nullptr), scriptMode_(CLASSIC), runnerState_(STARTING), runner_(nullptr),
      mainEngine_(env), workerEngine_(nullptr), workerWrapper_(thisVar), parentPort_(nullptr)
{}

void Worker::StartExecuteInThread(napi_env env, const char* script)
{
    // 1. init mainOnMessageSignal_ in main loop
    auto engine = reinterpret_cast<NativeEngine*>(env);
    uv_loop_t* loop = engine->GetUVLoop();
    if (loop == nullptr) {
        napi_throw_error(env, nullptr, "worker::engine loop is null");
        return;
    }
    uv_async_init(loop, &mainOnMessageSignal_, reinterpret_cast<uv_async_cb>(Worker::MainOnMessage));
    uv_async_init(loop, &mainOnErrorSignal_, reinterpret_cast<uv_async_cb>(Worker::MainOnError));

    // 2. copy the script
    script_ = strdup(script);
    CloseHelp::DeletePointer(script, true);

    // 4. create WorkerRunner to Execute
    if (runner_ == nullptr) {
        runner_ = new WorkerRunner(WorkerStartCallback(ExecuteInThread, this));
    }
    runner_->Execute(); // start a new thread
}

void Worker::CloseInner()
{
    UpdateWorkerState(TERMINATEING);
    TerminateWorker();
}

napi_value Worker::CloseWorker(napi_env env, napi_callback_info cbinfo)
{
    Worker* worker = nullptr;
    napi_get_cb_info(env, cbinfo, nullptr, nullptr, nullptr, (void**)&worker);
    if (worker != nullptr) {
        worker->CloseInner();
    }
    return NapiValueHelp::GetUndefinedValue(env);
}

void CallWorkCallback(napi_env env, napi_value recv, size_t argc, const napi_value* argv, const char* type)
{
    napi_value callback = nullptr;
    napi_get_named_property(env, recv, type, &callback);
    if (NapiValueHelp::IsCallable(env, callback)) {
        napi_value callbackResult = nullptr;
        napi_call_function(env, recv, callback, 1, argv, &callbackResult);
    }
}

void Worker::PrepareForWorkerInstance(const Worker* worker)
{
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(worker->GetWorkerEngine()));
    // 1. init worker environment
    if (OHOS::CCRuntime::Worker::WorkerCore::initWorkerFunc != NULL) {
        OHOS::CCRuntime::Worker::WorkerCore::initWorkerFunc(const_cast<NativeEngine*>(worker->GetWorkerEngine()));
    }
    // 2. Execute script
    if (OHOS::CCRuntime::Worker::WorkerCore::getAssertFunc == NULL) {
        HILOG_ERROR("worker::getAssertFunc is null");
        napi_throw_error(env, nullptr, "worker::getAssertFunc is null");
        return;
    }
    std::vector<uint8_t> scriptContent;
    OHOS::CCRuntime::Worker::WorkerCore::getAssertFunc(std::string(worker->GetScript()), scriptContent);
    std::string stringContent(scriptContent.begin(), scriptContent.end());
    HILOG_INFO("worker:: stringContent = %{private}s", stringContent.c_str());
    napi_value scriptStringNapiValue = nullptr;
    napi_create_string_utf8(env, stringContent.c_str(), stringContent.length(), &scriptStringNapiValue);

    napi_value execScriptResult = nullptr;
    napi_run_script(env, scriptStringNapiValue, &execScriptResult);
    if (execScriptResult == nullptr) {
        // An exception occurred when running the script.
        HILOG_ERROR("worker:: run script exception occurs, will handle exception");
        (const_cast<Worker*>(worker))->HandleException(worker->GetWorkerEngine());
        return;
    }

    // 3. register postMessage in DedicatedWorkerGlobalScope
    napi_value postFunctionObj = nullptr;
    napi_create_function(env, "postMessage", NAPI_AUTO_LENGTH, Worker::PostMessageToMain,
        const_cast<Worker*>(worker), &postFunctionObj);
    NapiValueHelp::SetNamePropertyInGlobal(env, "postMessage", postFunctionObj);
    // 4. register close in DedicatedWorkerGlobalScope
    napi_value closeFuncObj = nullptr;
    napi_create_function(env, "close", NAPI_AUTO_LENGTH, Worker::CloseWorker,
        const_cast<Worker*>(worker), &closeFuncObj);
    NapiValueHelp::SetNamePropertyInGlobal(env, "close", closeFuncObj);
    // 5. register worker name in DedicatedWorkerGlobalScope
    if (worker->GetName() != nullptr) {
        napi_value nameValue = nullptr;
        napi_create_string_utf8(env, worker->GetName(), strlen(worker->GetName()), &nameValue);
        NapiValueHelp::SetNamePropertyInGlobal(env, "name", nameValue);
    }
}

bool Worker::UpdateWorkerState(RunnerState state)
{
    bool done = false;
    do {
        RunnerState oldState = runnerState_.load(std::memory_order_acquire);
        if (oldState < state) {
            done = runnerState_.compare_exchange_strong(oldState, state);
        } else {
            return false;
        }
    } while (!done);
    return true;
}

void Worker::PublishWorkerOverSignal()
{
    // post NULL tell main worker is not running
    mainMessageQueue_.EnQueue(NULL);
    uv_async_send(&mainOnMessageSignal_);
    mainEngine_->TriggerPostTask();
}

void Worker::ExecuteInThread(const void* data)
{
    auto worker = reinterpret_cast<Worker*>(const_cast<void*>(data));
    // 1. create a runtime, nativeengine
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(worker->GetMainEngine()));
    napi_env newEnv = nullptr;
    napi_create_runtime(env, &newEnv);
    if (newEnv == nullptr) {
        napi_throw_error(env, nullptr, "Worker create runtime error");
        return;
    }
    NativeEngine *workerEngine = reinterpret_cast<NativeEngine*>(newEnv);
    // mark worker env is subThread
    workerEngine->MarkSubThread();
    worker->SetWorkerEngine(workerEngine);

    uv_loop_t* loop = worker->GetWorkerLoop();
    if (loop == nullptr) {
        napi_throw_error(env, nullptr, "Worker loop is nullptr");
        return;
    }
    uv_async_init(loop, &worker->workerOnMessageSignal_, reinterpret_cast<uv_async_cb>(Worker::WorkerOnMessage));

    if (worker->UpdateWorkerState(RUNNING)) {
        // 2. add some preparation for the worker
        PrepareForWorkerInstance(worker);
        // 3. start worker loop
        const NativeEngine* workerEngine = worker->GetWorkerEngine();
        if (workerEngine == nullptr) {
            HILOG_ERROR("worker::worker engine is null");
        } else {
            uv_async_send(&worker->workerOnMessageSignal_);
            const_cast<NativeEngine*>(workerEngine)->Loop(LOOP_DEFAULT);
        }
    } else {
        worker->CloseInner();
    }
    worker->PublishWorkerOverSignal();
}

void Worker::MainOnMessage(const uv_async_t* req)
{
    Worker* worker = DereferenceHelp::DereferenceOf(&Worker::mainOnMessageSignal_, req);
    if (worker == nullptr) {
        HILOG_ERROR("worker::worker is null");
        return;
    }
    worker->MainOnMessageInner(worker->GetMainEngine());
}

void Worker::MainOnErrorInner(const NativeEngine* engine)
{
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));
    napi_value callback = nullptr;
    napi_value obj = nullptr;
    napi_get_reference_value(env, workerWrapper_, &obj);
    napi_get_named_property(env, obj, "onerror", &callback);
    bool isCallable = NapiValueHelp::IsCallable(env, callback);
    if (!isCallable) {
        HILOG_ERROR("worker:: worker onerror is not Callable");
        return;
    }
    MessageDataType data;
    while (errorQueue_.DeQueue(&data)) {
        napi_value result = nullptr;
        napi_deserialize(env, data, &result);

        napi_value argv[1] = { result };
        napi_value callbackResult = nullptr;
        napi_call_function(env, obj, callback, 1, argv, &callbackResult);

        // handle listeners
        HandleEventListeners(env, obj, 1, argv, "error");
    }
}

void Worker::MainOnError(const uv_async_t* req)
{
    Worker* worker = DereferenceHelp::DereferenceOf(&Worker::mainOnErrorSignal_, req);
    if (worker == nullptr) {
        HILOG_ERROR("worker::worker is null");
        return;
    }
    worker->MainOnErrorInner(worker->GetMainEngine());
    worker->TerminateInner();
}

void Worker::WorkerOnMessage(const uv_async_t* req)
{
    Worker* worker = DereferenceHelp::DereferenceOf(&Worker::workerOnMessageSignal_, req);
    if (worker == nullptr) {
        HILOG_ERROR("worker::worker is null");
        return;
    }
    worker->WorkerOnMessageInner(worker->GetWorkerEngine());
}

void Worker::CloseMainCallback() const
{
    napi_value exitValue = nullptr;
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(GetMainEngine()));
    napi_create_int32(env, 1, &exitValue);
    napi_value argv[1] = { exitValue };
    CallMainFunction(GetMainEngine(), 1, argv, "onexit");

    std::lock_guard<std::mutex> lock(g_workersMutex);
    std::list<Worker*>::iterator it = std::find(g_workers.begin(), g_workers.end(), this);
    if (it != g_workers.end()) {
        g_workers.erase(it);
    }
    CloseHelp::DeletePointer(this, false);
}

void Worker::HandleEventListeners(napi_env env, napi_value recv, size_t argc, const napi_value* argv, const char* type)
{
    std::string listener(type);
    auto iter = eventListeners_.find(listener);
    if (iter == eventListeners_.end()) {
        HILOG_INFO("worker:: there is no listener for type %{public}s", type);
        return;
    }

    std::list<WorkerListener*>& listeners = iter->second;
    std::list<WorkerListener*>::iterator it = listeners.begin();
    while (it != listeners.end()) {
        WorkerListener* data = *it++;
        napi_ref callback = data->GetCallback();
        napi_value callbackObj = nullptr;
        napi_get_reference_value(env, callback, &callbackObj);
        napi_value callbackResult = nullptr;
        napi_call_function(env, recv, callbackObj, argc, argv, &callbackResult);
        if (!data->NextIsAvailable()) {
            listeners.remove(data);
            CloseHelp::DeletePointer(data, false);
        }
    }
}

void Worker::MainOnMessageInner(const NativeEngine* engine)
{
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));

    napi_value callback = nullptr;
    napi_value obj = nullptr;
    napi_get_reference_value(env, workerWrapper_, &obj);
    napi_get_named_property(env, obj, "onmessage", &callback);
    bool isCallable = NapiValueHelp::IsCallable(env, callback);

    MessageDataType data = nullptr;
    while (mainMessageQueue_.DeQueue(&data)) {
        // receive close signal.
        if (data == nullptr) {
            HILOG_INFO("worker:: worker received close signal");
            uv_unref((uv_handle_t*)&mainOnMessageSignal_);
            uv_close((uv_handle_t*)&mainOnMessageSignal_, nullptr);

            uv_unref((uv_handle_t*)&mainOnErrorSignal_);
            uv_close((uv_handle_t*)&mainOnErrorSignal_, nullptr);
            CloseMainCallback();
            return;
        }
        if (!isCallable) {
            // onmessage is not func, no need to continue
            HILOG_ERROR("worker:: worker onmessage is not a callable");
            return;
        }
        // handle data, call worker onMessage function to handle.
        napi_value result = nullptr;
        napi_deserialize(env, data, &result);
        napi_value event = nullptr;
        napi_create_object(env, &event);
        napi_set_named_property(env, event, "data", result);
        napi_value argv[1] = { event };
        napi_value callbackResult = nullptr;
        napi_call_function(env, obj, callback, 1, argv, &callbackResult);
        // handle listeners.
        HandleEventListeners(env, obj, 1, argv, "message");
    }
}

void Worker::TerminateWorker()
{
    // when there is no active handle, worker loop will stop automatic.
    std::lock_guard<std::mutex> lock(workerAsyncMutex_);
    uv_close((uv_handle_t*)&workerOnMessageSignal_, nullptr);
    CloseWorkerCallback();
    uv_loop_t* loop = GetWorkerLoop();
    if (loop != nullptr) {
        uv_stop(loop);
    }
    UpdateWorkerState(TERMINATED);
}

void Worker::HandleException(const NativeEngine* engine)
{
    // obj.message, obj.filename, obj.lineno, obj.colno
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));
    napi_value exception = nullptr;
    napi_create_object(env, &exception);

    napi_get_exception_info_for_worker(env, exception);

    // add obj.filename
    napi_value filenameValue = nullptr;
    napi_create_string_utf8(env, script_, strlen(script_), &filenameValue);
    napi_set_named_property(env, exception, "filename", filenameValue);

    // WorkerGlobalScope onerror
    WorkerOnErrorInner(engine, exception);

    if (mainEngine_ != nullptr) {
        napi_value data = nullptr;
        napi_serialize(env, exception, NapiValueHelp::GetUndefinedValue(env), &data);
        errorQueue_.EnQueue(data);
        uv_async_send(&mainOnErrorSignal_);
        mainEngine_->TriggerPostTask();
    } else {
        HILOG_ERROR("worker:: main engine is nullptr.");
    }
}

void Worker::WorkerOnMessageInner(const NativeEngine* engine)
{
    if (IsTerminated()) {
        return;
    }
    MessageDataType data = nullptr;
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));
    while (workerMessageQueue_.DeQueue(&data)) {
        if (data == NULL || IsTerminating()) {
            HILOG_INFO("worker:: worker reveive terminate signal");
            TerminateWorker();
            return;
        }
        napi_value result = nullptr;
        napi_status status = napi_deserialize(env, data, &result);
        if (status != napi_ok || result == nullptr) {
            WorkerOnMessageErrorInner(workerEngine_);
            return;
        }

        napi_value event = nullptr;
        napi_create_object(env, &event);
        napi_set_named_property(env, event, "data", result);
        napi_value argv[1] = { event };
        bool callFeedback = CallWorkerFunction(engine, 1, argv, "onmessage", true);
        if (!callFeedback) {
            // onmessage is not function, exit the loop directly.
            return;
        }
    }
}

void Worker::MainOnMessageErrorInner(const NativeEngine* engine)
{
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));
    napi_value obj = nullptr;
    napi_get_reference_value(env, workerWrapper_, &obj);
    CallMainFunction(engine, 0, nullptr, "onmessageerror");
    // handle listeners
    HandleEventListeners(env, obj, 0, nullptr, "messageerror");
}

void Worker::WorkerOnMessageErrorInner(const NativeEngine* engine)
{
    CallWorkerFunction(engine, 0, nullptr, "onmessageerror", true);
}

napi_value Worker::PostMessage(napi_env env, napi_callback_info cbinfo)
{
    size_t argc = NapiValueHelp::GetCallbackInfoArgc(env, cbinfo);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Worker param count must be more than 1 with new");
        return nullptr;
    }
    napi_value* argv = new napi_value[argc];
    [[maybe_unused]] ObjectScope<napi_value> scope(argv, true);
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, cbinfo, &argc, argv, &thisVar, nullptr);
    Worker* worker = nullptr;
    napi_unwrap(env, thisVar, (void**)&worker);

    if (worker == nullptr) {
        HILOG_ERROR("worker:: worker is nullptr when PostMessage, maybe worker is terminated");
        return nullptr;
    }

    if (worker->IsTerminated() || worker->IsTerminating()) {
        HILOG_INFO("worker:: worker not in running state");
        return nullptr;
    }

    napi_value data = nullptr;
    napi_status serializeStatus = napi_ok;
    if (argc >= WORKERPARAMNUM) {
        if (!NapiValueHelp::IsArray(argv[1])) {
            napi_throw_error(env, nullptr, "Transfer list must be an Array");
            return nullptr;
        }
        serializeStatus = napi_serialize(env, argv[0], argv[1], &data);
    } else {
        serializeStatus = napi_serialize(env, argv[0], NapiValueHelp::GetUndefinedValue(env), &data);
    }
    if (serializeStatus != napi_ok || data == nullptr) {
        worker->MainOnMessageErrorInner(worker->GetMainEngine());
        return nullptr;
    }

    if (data != nullptr) {
        worker->PostMessageInner(data);
    }

    return NapiValueHelp::GetUndefinedValue(env);
}

napi_value Worker::PostMessageToMain(napi_env env, napi_callback_info cbinfo)
{
    size_t argc = NapiValueHelp::GetCallbackInfoArgc(env, cbinfo);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Worker param count must be more than 1 with new");
        return nullptr;
    }
    napi_value* argv = new napi_value[argc];
    [[maybe_unused]] ObjectScope<napi_value> scope(argv, true);
    Worker* worker = nullptr;
    napi_get_cb_info(env, cbinfo, &argc, argv, nullptr, (void**)&worker);

    if (worker == nullptr) {
        HILOG_ERROR("worker:: when post message to main occur worker is nullptr");
        return nullptr;
    }

    if (!worker->IsRunning()) {
        // if worker is not running, don't send any message to main thread
        HILOG_INFO("worker:: when post message to main occur worker is not in running.");
        return nullptr;
    }

    napi_value data = nullptr;
    if (argc >= WORKERPARAMNUM) {
        if (!NapiValueHelp::IsArray(argv[1])) {
            napi_throw_error(env, nullptr, "Transfer list must be an Array");
            return nullptr;
        }
        napi_serialize(env, argv[0], argv[1], &data);
    } else {
        napi_serialize(env, argv[0], NapiValueHelp::GetUndefinedValue(env), &data);
    }

    if (data != nullptr) {
        worker->PostMessageToMainInner(data);
    }

    return NapiValueHelp::GetUndefinedValue(env);
}

void Worker::PostMessageToMainInner(MessageDataType data)
{
    if (mainEngine_ != nullptr) {
        mainMessageQueue_.EnQueue(data);
        uv_async_send(&mainOnMessageSignal_);
        mainEngine_->TriggerPostTask();
    } else {
        HILOG_ERROR("worker:: worker main engine is nullptr.");
    }
}

void Worker::PostMessageInner(MessageDataType data)
{
    if (IsTerminating()) {
        HILOG_INFO("worker:: worker is terminating, will not handle andy worker.");
        return;
    }
    if (IsTerminated()) {
        HILOG_INFO("worker:: worker has been terminated.");
        return;
    }
    std::lock_guard<std::mutex> lock(workerAsyncMutex_);
    workerMessageQueue_.EnQueue(data);
    if (IsRunning()) {
        uv_async_send(&workerOnMessageSignal_);
    }
}

napi_value Worker::Terminate(napi_env env, napi_callback_info cbinfo)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisVar, nullptr);
    Worker* worker = nullptr;
    napi_unwrap(env, thisVar, (void**)&worker);
    if (worker == nullptr) {
        HILOG_ERROR("worker:: worker is nullptr when Terminate, maybe worker is terminated");
        return nullptr;
    }
    if (worker->IsTerminated() || worker->IsTerminating()) {
        HILOG_INFO("worker:: worker is not in running");
        return nullptr;
    }
    worker->TerminateInner();
    return NapiValueHelp::GetUndefinedValue(env);
}

void Worker::TerminateInner()
{
    // 1. send null signal
    PostMessageInner(NULL);
    UpdateWorkerState(TERMINATEING);
}

Worker::~Worker()
{
    CloseHelp::DeletePointer(script_, true);
    script_ = nullptr;
    CloseHelp::DeletePointer(name_, true);
    name_ = nullptr;
    napi_env env = reinterpret_cast<napi_env>(mainEngine_);
    workerMessageQueue_.Clear(env);
    mainMessageQueue_.Clear(env);
    // set thisVar's nativepointer is null
    napi_value thisVar = nullptr;
    napi_get_reference_value(env, workerWrapper_, &thisVar);
    Worker* worker = nullptr;
    napi_remove_wrap(env, thisVar, (void**)&worker);

    napi_delete_reference(env, workerWrapper_);
    workerWrapper_ = nullptr;

    napi_delete_reference(env, parentPort_);
    parentPort_ = nullptr;

    CloseHelp::DeletePointer(runner_, false);
    runner_ = nullptr;

    CloseHelp::DeletePointer(workerEngine_, false);
    workerEngine_ = nullptr;

    mainEngine_ = nullptr;
    RemoveAllListenerInner();
}

napi_value Worker::WorkerConstructor(napi_env env, napi_callback_info cbinfo)
{
    // check argv count
    size_t argc = NapiValueHelp::GetCallbackInfoArgc(env, cbinfo);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Worker param count must be more than 1 with new");
        return nullptr;
    }

    // check 1st param is string
    napi_value thisVar = nullptr;
    void* data = nullptr;
    napi_value* args = new napi_value[argc];
    [[maybe_unused]] ObjectScope<napi_value> scope(args, true);
    napi_get_cb_info(env, cbinfo, &argc, args, &thisVar, &data);
    if (!NapiValueHelp::IsString(args[0])) {
        napi_throw_error(env, nullptr, "Worker 1st param must be string with new");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_workersMutex);
    if (g_workers.size() >= MAXWORKERS) {
        napi_throw_error(env, nullptr, "Too many workers, the number of workers exceeds the maximum.");
        return nullptr;
    }

    // 2. new worker instance
    NativeEngine* engine = reinterpret_cast<NativeEngine*>(env);
    Worker* worker = new Worker(engine, nullptr);
    g_workers.push_back(worker);

    if (argc > 1 && NapiValueHelp::IsObject(args[1])) {
        napi_value nameValue = nullptr;
        napi_get_named_property(env, args[1], "name", &nameValue);
        if (NapiValueHelp::IsString(nameValue)) {
            char* nameStr = NapiValueHelp::GetString(env, nameValue);
            if (nameStr == nullptr) {
                napi_throw_error(env, nullptr, "worker name create error, please check.");
                return nullptr;
            }
            worker->name_ = strdup(nameStr);
            CloseHelp::DeletePointer(nameStr, true);
        }

        napi_value typeValue = nullptr;
        napi_get_named_property(env, args[1], "type", &typeValue);
        if (NapiValueHelp::IsString(typeValue)) {
            char* typeStr = NapiValueHelp::GetString(env, typeValue);
            if (typeStr == nullptr) {
                napi_throw_error(env, nullptr, "worker type create error, please check.");
                return nullptr;
            }
            if (strcmp("classic", typeStr) == 0) {
                worker->SetScriptMode(CLASSIC);
                CloseHelp::DeletePointer(typeStr, true);
            } else if (strcmp("module", typeStr) == 0) {
                worker->SetScriptMode(MODULE);
                napi_throw_error(env, nullptr, "unsupport module");
                CloseHelp::DeletePointer(typeStr, true);
                CloseHelp::DeletePointer(worker, false);
                return nullptr;
            } else {
                worker->SetScriptMode(MODULE);
                napi_throw_error(env, nullptr, "unsupport module");
                CloseHelp::DeletePointer(typeStr, true);
                CloseHelp::DeletePointer(worker, false);
                return nullptr;
            }
        }
    }

    // 3. execute StartExecuteInThread;
    char* script = NapiValueHelp::GetString(env, args[0]);
    if (script == nullptr) {
        napi_throw_error(env, nullptr, "worker script create error, please check.");
        return nullptr;
    }
    HILOG_INFO("worker:: script is %{public}s", script);
    worker->StartExecuteInThread(env, script);
    napi_wrap(
        env, thisVar, worker,
        [](napi_env env, void* data, void* hint) {
            Worker* worker = (Worker*)data;
            auto iter = std::find(g_workers.begin(), g_workers.end(), worker);
            if (iter == g_workers.end()) {
                return;
            }
            if (worker->IsTerminated() || worker->IsTerminating()) {
                HILOG_INFO("worker:: worker is not in running");
                return;
            }
            worker->TerminateInner();
        },
        nullptr, nullptr);
    napi_create_reference(env, thisVar, 1, &worker->workerWrapper_);
    return thisVar;
}

napi_value Worker::AddListener(napi_env env, napi_callback_info cbinfo, ListenerMode mode)
{
    size_t argc = NapiValueHelp::GetCallbackInfoArgc(env, cbinfo);
    if (argc < WORKERPARAMNUM) {
        napi_throw_error(env, nullptr, "Worker param count must be more than WORKPARAMNUM with on");
        return nullptr;
    }
    // check 1st param is string
    napi_value thisVar = nullptr;
    void* data = nullptr;
    napi_value* args = new napi_value[argc];
    [[maybe_unused]] ObjectScope<napi_value> scope(args, true);
    napi_get_cb_info(env, cbinfo, &argc, args, &thisVar, &data);
    if (!NapiValueHelp::IsString(args[0])) {
        napi_throw_error(env, nullptr, "Worker 1st param must be string with on");
        return nullptr;
    }
    if (!NapiValueHelp::IsCallable(env, args[1])) {
        napi_throw_error(env, nullptr, "Worker 2st param must be callable with on");
        return nullptr;
    }
    Worker* worker = nullptr;
    napi_unwrap(env, thisVar, (void**)&worker);
    if (worker == nullptr) {
        HILOG_ERROR("worker:: worker is nullptr when addListener, maybe worker is terminated");
        return nullptr;
    }

    auto listener = new WorkerListener(worker, mode);
    if (mode == ONCE && argc > WORKERPARAMNUM) {
        if (NapiValueHelp::IsObject(args[2])) {
            napi_value onceValue = nullptr;
            napi_get_named_property(env, args[2], "once", &onceValue);
            bool isOnce = false;
            napi_get_value_bool(env, onceValue, &isOnce);
            if (!isOnce) {
                listener->SetMode(PERMANENT);
            }
        }
    }
    listener->SetCallable(env, args[1]);
    char* typeStr = NapiValueHelp::GetString(env, args[0]);
    if (typeStr == nullptr) {
        CloseHelp::DeletePointer(listener, false);
        napi_throw_error(env, nullptr, "worker listener type create error, please check.");
        return nullptr;
    }
    worker->AddListenerInner(env, typeStr, listener);
    CloseHelp::DeletePointer(typeStr, true);
    return NapiValueHelp::GetUndefinedValue(env);
}

bool Worker::WorkerListener::operator==(const WorkerListener& listener) const
{
    if (listener.worker_ == nullptr) {
        return false;
    }
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(listener.worker_->GetMainEngine()));
    napi_ref ref = listener.GetCallback();
    napi_value obj = nullptr;
    napi_get_reference_value(env, ref, &obj);

    napi_value compareObj = nullptr;
    napi_get_reference_value(env, callback_, &compareObj);
    return obj == compareObj;
}

void Worker::AddListenerInner(napi_env env, const char* type, const WorkerListener* listener)
{
    std::string typestr(type);
    auto iter = eventListeners_.find(typestr);
    if (iter == eventListeners_.end()) {
        std::list<WorkerListener*> listeners;
        listeners.emplace_back(const_cast<WorkerListener*>(listener));
        eventListeners_[typestr] = listeners;
    } else {
        std::list<WorkerListener*>& listenerList = iter->second;
        std::list<WorkerListener*>::iterator it = std::find_if(
            listenerList.begin(), listenerList.end(), Worker::FindWorkerListener(env, listener->GetCallback()));
        if (it != listenerList.end()) {
            return;
        }
        listenerList.emplace_back(const_cast<WorkerListener*>(listener));
    }
}

void Worker::RemoveListenerInner(napi_env env, const char* type, napi_ref callback)
{
    std::string typestr(type);
    auto iter = eventListeners_.find(typestr);
    if (iter == eventListeners_.end()) {
        return;
    }
    std::list<WorkerListener*>& listenerList = iter->second;
    if (callback != nullptr) {
        std::list<WorkerListener*>::iterator it =
            std::find_if(listenerList.begin(), listenerList.end(), Worker::FindWorkerListener(env, callback));
        if (it != listenerList.end()) {
            listenerList.erase(it);
            CloseHelp::DeletePointer(*it, false);
        }
    } else {
        for (auto it = listenerList.begin(); it != listenerList.end(); it++) {
            CloseHelp::DeletePointer(*it, false);
        }
        eventListeners_.erase(typestr);
    }
}

napi_value Worker::On(napi_env env, napi_callback_info cbinfo)
{
    return AddListener(env, cbinfo, PERMANENT);
}

napi_value Worker::Once(napi_env env, napi_callback_info cbinfo)
{
    return AddListener(env, cbinfo, ONCE);
}

napi_value Worker::RemoveListener(napi_env env, napi_callback_info cbinfo)
{
    size_t argc = NapiValueHelp::GetCallbackInfoArgc(env, cbinfo);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Worker param count must be more than 2 with on");
        return nullptr;
    }
    // check 1st param is string
    napi_value thisVar = nullptr;
    void* data = nullptr;
    napi_value* args = new napi_value[argc];
    [[maybe_unused]] ObjectScope<napi_value> scope(args, true);
    napi_get_cb_info(env, cbinfo, &argc, args, &thisVar, &data);
    if (!NapiValueHelp::IsString(args[0])) {
        napi_throw_error(env, nullptr, "Worker 1st param must be string with on");
        return nullptr;
    }

    Worker* worker = nullptr;
    napi_unwrap(env, thisVar, (void**)&worker);
    if (worker == nullptr) {
        HILOG_ERROR("worker:: worker is nullptr when RemoveListener, maybe worker is terminated");
        return nullptr;
    }

    napi_ref callback = nullptr;
    if (argc > 1 && !NapiValueHelp::IsCallable(env, args[1])) {
        napi_throw_error(env, nullptr, "Worker 2st param must be callable with on");
        return nullptr;
    }
    if (argc > 1 && NapiValueHelp::IsCallable(env, args[1])) {
        napi_create_reference(env, args[1], 1, &callback);
    }

    char* typeStr = NapiValueHelp::GetString(env, args[0]);
    if (typeStr == nullptr) {
        napi_throw_error(env, nullptr, "worker listener type create error, please check.");
        return nullptr;
    }
    worker->RemoveListenerInner(env, typeStr, callback);
    CloseHelp::DeletePointer(typeStr, true);
    napi_delete_reference(env, callback);
    return NapiValueHelp::GetUndefinedValue(env);
}

napi_value Worker::Off(napi_env env, napi_callback_info cbinfo)
{
    return RemoveListener(env, cbinfo);
}

napi_value Worker::AddEventListener(napi_env env, napi_callback_info cbinfo)
{
    return AddListener(env, cbinfo, PERMANENT);
}

napi_value Worker::DispatchEvent(napi_env env, napi_callback_info cbinfo)
{
    size_t argc = NapiValueHelp::GetCallbackInfoArgc(env, cbinfo);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "worker:: DispatchEvent param count must be more than 1");
        return NapiValueHelp::GetBooleanValue(env, false);
    }

    // check 1st param is string
    napi_value thisVar = nullptr;
    void* data = nullptr;
    napi_value* args = new napi_value[argc];
    [[maybe_unused]] ObjectScope<napi_value> scope(args, true);
    napi_get_cb_info(env, cbinfo, &argc, args, &thisVar, &data);

    if (!NapiValueHelp::IsObject(args[0])) {
        napi_throw_error(env, nullptr, "worker DispatchEvent 1st param must be Event");
        return NapiValueHelp::GetBooleanValue(env, false);
    }

    Worker* worker = nullptr;
    napi_unwrap(env, thisVar, (void**)&worker);
    if (worker == nullptr) {
        HILOG_ERROR("worker:: worker is nullptr when DispatchEvent, maybe worker is terminated");
        return NapiValueHelp::GetBooleanValue(env, false);
    }

    napi_value typeValue = nullptr;
    napi_get_named_property(env, args[0], "type", &typeValue);
    if (!NapiValueHelp::IsString(typeValue)) {
        napi_throw_error(env, nullptr, "worker event type must be string");
        return NapiValueHelp::GetBooleanValue(env, false);
    }

    napi_value obj = nullptr;
    napi_get_reference_value(env, worker->workerWrapper_, &obj);
    napi_value argv[1] = { args[0] };

    char* typeStr = NapiValueHelp::GetString(env, typeValue);
    if (typeStr == nullptr) {
        napi_throw_error(env, nullptr, "worker listener type create error, please check.");
        return NapiValueHelp::GetBooleanValue(env, false);
    }
    if (strcmp(typeStr, "error") == 0) {
        CallWorkCallback(env, obj, 1, argv, "onerror");
    } else if (strcmp(typeStr, "messageerror") == 0) {
        CallWorkCallback(env, obj, 1, argv, "onmessageerror");
    } else if (strcmp(typeStr, "message") == 0) {
        CallWorkCallback(env, obj, 1, argv, "onmessage");
    }

    worker->HandleEventListeners(env, obj, 1, argv, typeStr);

    CloseHelp::DeletePointer(typeStr, true);
    return NapiValueHelp::GetBooleanValue(env, true);
}

napi_value Worker::RemoveEventListener(napi_env env, napi_callback_info cbinfo)
{
    return RemoveListener(env, cbinfo);
}

void Worker::RemoveAllListenerInner()
{
    for (auto iter = eventListeners_.begin(); iter != eventListeners_.end(); iter++) {
        std::list<WorkerListener*>& listeners = iter->second;
        for (auto item = listeners.begin(); item != listeners.end(); item++) {
            WorkerListener* listener = *item;
            CloseHelp::DeletePointer(listener, false);
        }
    }
    eventListeners_.clear();
}

napi_value Worker::RemoveAllListener(napi_env env, napi_callback_info cbinfo)
{
    napi_value thisVar = nullptr;
    napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisVar, nullptr);
    Worker* worker = nullptr;
    napi_unwrap(env, thisVar, (void**)&worker);
    if (worker == nullptr) {
        HILOG_ERROR("worker:: worker is nullptr when RemoveAllListener, maybe worker is terminated");
        return nullptr;
    }

    worker->RemoveAllListenerInner();
    return NapiValueHelp::GetUndefinedValue(env);
}

napi_value Worker::InitWorker(napi_env env, napi_value exports)
{
    NativeEngine *engine = reinterpret_cast<NativeEngine*>(env);
    if (engine->IsMainThread()) {
        const char className[] = "Worker";
        napi_property_descriptor properties[] = {
            DECLARE_NAPI_FUNCTION("postMessage", PostMessage),
            DECLARE_NAPI_FUNCTION("terminate", Terminate),
            DECLARE_NAPI_FUNCTION("on", On),
            DECLARE_NAPI_FUNCTION("once", Once),
            DECLARE_NAPI_FUNCTION("off", Off),
            DECLARE_NAPI_FUNCTION("addEventListener", AddEventListener),
            DECLARE_NAPI_FUNCTION("dispatchEvent", DispatchEvent),
            DECLARE_NAPI_FUNCTION("removeEventListener", RemoveEventListener),
            DECLARE_NAPI_FUNCTION("removeAllListener", RemoveAllListener),
        };
        napi_value workerClazz = nullptr;
        napi_define_class(env, className, sizeof(className), Worker::WorkerConstructor, nullptr,
            sizeof(properties) / sizeof(properties[0]), properties, &workerClazz);
        napi_set_named_property(env, exports, "Worker", workerClazz);
    } else {
        NativeEngine *engine = reinterpret_cast<NativeEngine*>(env);
        Worker *worker = nullptr;
        for (auto item = g_workers.begin(); item != g_workers.end(); item++) {
            if ((*item)->GetWorkerEngine() == engine) {
                worker = *item;
            }
        }
        if (worker == nullptr) {
            napi_throw_error(env, nullptr, "worker:: worker is null");
            return exports;
        }

        napi_property_descriptor properties[] = {
            DECLARE_NAPI_FUNCTION_WITH_DATA("postMessage", PostMessageToMain, worker),
            DECLARE_NAPI_FUNCTION_WITH_DATA("close", CloseWorker, worker),
        };
        const char propertyName[] = "parentPort";
        napi_value parentPortObj = nullptr;
        napi_create_object(env, &parentPortObj);
        napi_define_properties(env, parentPortObj, sizeof(properties) / sizeof(properties[0]), properties);
        napi_set_named_property(env, exports, propertyName, parentPortObj);

        // register worker parentPort.
        napi_create_reference(env, parentPortObj, 1, &worker->parentPort_);
    }
    return exports;
}

void Worker::WorkerOnErrorInner(const NativeEngine* engine, napi_value error)
{
    napi_value argv[1] = { error };
    CallWorkerFunction(engine, 1, argv, "onerror", false);
}

bool Worker::CallWorkerFunction(
    const NativeEngine* engine, int argc, const napi_value* argv, const char* methodName, bool tryCatch)
{
    if (engine == nullptr) {
        return false;
    }
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));
    napi_value callback = NapiValueHelp::GetNamePropertyInParentPort(env, parentPort_, methodName);
    bool isCallable = NapiValueHelp::IsCallable(env, callback);
    if (!isCallable) {
        HILOG_ERROR("worker:: WorkerGlobalScope %{public}s is not Callable", methodName);
        return false;
    }
    napi_value undefinedValue = NapiValueHelp::GetUndefinedValue(env);
    napi_value callbackResult = nullptr;
    napi_call_function(env, undefinedValue, callback, argc, argv, &callbackResult);
    if (tryCatch && callbackResult == nullptr) {
        // handle exception
        HandleException(GetWorkerEngine());
    }
    return true;
}

void Worker::CloseWorkerCallback()
{
    CallWorkerFunction(GetWorkerEngine(), 0, nullptr, "onclose", true);
    // off worker inited environment
    if (OHOS::CCRuntime::Worker::WorkerCore::offWorkerFunc != NULL) {
        OHOS::CCRuntime::Worker::WorkerCore::offWorkerFunc(const_cast<NativeEngine*>(GetWorkerEngine()));
    }
}

void Worker::CallMainFunction(
    const NativeEngine* engine, int argc, const napi_value* argv, const char* methodName) const
{
    if (engine == nullptr) {
        return;
    }
    napi_env env = reinterpret_cast<napi_env>(const_cast<NativeEngine*>(engine));
    napi_value callback = nullptr;
    napi_value obj = nullptr;
    napi_get_reference_value(env, workerWrapper_, &obj);
    napi_get_named_property(env, obj, methodName, &callback);
    bool isCallable = NapiValueHelp::IsCallable(env, callback);
    if (!isCallable) {
        HILOG_ERROR("worker:: worker %{public}s is not Callable", methodName);
        return;
    }
    napi_value callbackResult = nullptr;
    napi_call_function(env, obj, callback, argc, argv, &callbackResult);
}
} // namespace OHOS::CCRuntime::Worker
