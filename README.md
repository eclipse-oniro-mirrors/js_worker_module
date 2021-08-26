# js_worker_module

#### 一、Worker介绍

worker能够让js拥有多线程的能力，通过postMessage完成worker线程与宿主线程通信。

一. Worker介绍

接口介绍

1.constructor(scriptURL:string, options? WorkerOptions);
构造函数 

使用方法:
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");

2. postMessage(message:Object, options?:PostMessageOptions): void;
描述:
向worker线程发送消息，数据的传输采用结构化算法

使用方法:

示例一
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.postMessage("hello world");

示例二
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
var buffer = new ArrayBuffer(8)
worker.postMessage(buffer, [buffer]);

3. on(type:string, listener:EventListener):void;
描述:
向worker添加一个事件监听

使用方法:
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.on("alert", (e)=>{
    console.log("worker on...");
})

4. once(type:string, listener:EventListener):void;
描述:
向worker添加一个事件监听, 事件监听只执行一次，一旦出发便会自动删除

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.once("alert", (e)=>{
    console.log("worker on...");
})

5. off(type:string, listener?:EventListener):void;
描述:
删除worker的事件监听

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.off("alert");

6. terminate():void;
描述:
关闭worker线程，终止worker发送消息

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.terminate();

7. removeEventListener(type:string, listener?:EventListener):void;
描述:
删除worker的事件监听

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.removeEventListener("alert");

8. dispatchEvent(event: Event):boolean;
描述:
分发worker的事件监听

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.dispatchEvent({type:"alert"});

9. removeAllEventListener(): void;
描述:
删除worker的所有事件监听

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.removeAllEventListener();

10. onexit?:(code:number)=>void;
描述:
worker退出时出发js线程的回调方法

使用方法
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.onexit = function(e) {
    console.log("onexit...");
}

11. onerror?:(ev:ErrorEvent)=>void;
描述:
worker内部执行js发生异常触发的宿主线程回调

使用方法:
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.onerror = function(e) {
    console.log("onerror...");
}

12. onmessage?:(ev:MessageEvent)=>void;
描述:
当宿主线程接受到来自其创建的worker消息时，会在worker对象上触发message事件。例如，当worker通过parentPort.postMessage()发送了一条消息

使用方法:
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.onmessage = function(e) {
    console.log("onmessage...");
}

13. onmessageerror?:(event:MessageEvent)=>void;
描述:
worker对象接收到一条无法序列化的消息时，messageerror事件将在该对象上被触发

使用方法:
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.onmessageerror = function(e) {
    console.log("onmessageerror...");
}

二. parentPort介绍
描述:
worker线程用于与宿主线程通信的Object对象，通过parentPort接口发送消息给主线程、close接口关闭worker线程

接口介绍

1. parent.postMessage(message:Object, options?:PostMessageOptions): void;
描述:
worker向宿主线程发送消息

使用方法:
main.js
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.postMessage("hello world");

worker.js
import worker from "@ohos.worker"
const parentPort = worker.parentPort;
parentPort.onmessage = function(e) {
    parentPort.postMessage("hello world from worker.js");
}

2. parent.close():void
描述:
关闭worker线程，终止worker接收消息

使用方法:
main.js
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.postMessage("hello world");

worker.js
import worker from "@ohos.worker"
const parentPort = worker.parentPort;
parentPort.onmessage = function(e) {
    parentPort.close();
}

3. parent.onmessage?:(event:MessageEvent)=>void
描述:
parent接口的onmessage属性表示在消息事件发生时要调用的事件处理程序，即当使用worker.postMessage方法将消息发送至worker时

使用方法：
main.js
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.postMessage("hello world");

worker.js
import worker from "@ohos.worker"
const parentPort = worker.parentPort;
parentPort.onmessage = function(e) {
    console.log("receive main.js message");
}

4. parentPort.onerror?:(ev: ErrorEvent)=>void;
描述:
worker线程内部执行js发生异常触发的worker回调

使用方法：
main.js
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.postMessage("hello world");

worker.js
import worker from "@ohos.worker"
const parentPort = worker.parentPort;
parentPort.onerror = function(e) {
    console.log("onerror...");
}

5. parentPort.onmessageerror?:(event: MessageEvent)=>void;
描述:
当worker接收到一条无法被反序列化的消息时，messageerror将在该事件上触发

使用方法：
main.js
import worker from "@ohos.worker"
const worker = new worker.Worker("workers/worker.js");
worker.postMessage("hello world");

worker.js
import worker from "@ohos.worker"
const parentPort = worker.parentPort;
parentPort.onmessageerror = function(e) {
    console.log("onmessageerror...");
}