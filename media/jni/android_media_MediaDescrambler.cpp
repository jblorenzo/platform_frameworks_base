/*
 * Copyright 2017, The Android Open Source Project
 *
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

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaDescrambler-JNI"
#include <utils/Log.h>

#include "android_media_MediaDescrambler.h"
#include "android_runtime/AndroidRuntime.h"
#include "android_os_HwRemoteBinder.h"
#include "JNIHelp.h"

#include <android/hardware/cas/native/1.0/BpHwDescrambler.h>
#include <android/hardware/cas/native/1.0/BnHwDescrambler.h>
#include <binder/MemoryDealer.h>
#include <hidl/HidlSupport.h>
#include <media/stagefright/foundation/ADebug.h>
#include <nativehelper/ScopedLocalRef.h>

namespace android {

using hardware::hidl_handle;

struct fields_t {
    jfieldID context;
};

static fields_t gFields;

static sp<JDescrambler> getDescrambler(JNIEnv *env, jobject thiz) {
    return (JDescrambler *)env->GetLongField(thiz, gFields.context);
}

static void setDescrambler(
        JNIEnv *env, jobject thiz, const sp<JDescrambler> &descrambler) {
    sp<JDescrambler> old = (JDescrambler *)env->GetLongField(thiz, gFields.context);
    if (descrambler != NULL) {
        descrambler->incStrong(thiz);
    }
    if (old != NULL) {
        old->decStrong(thiz);
    }
    env->SetLongField(thiz, gFields.context, (jlong)descrambler.get());
}

static status_t getBufferAndSize(
        JNIEnv *env, jobject byteBuf, jint offset, jint limit, size_t length,
        void **outPtr, jbyteArray *outByteArray) {
    void *ptr = env->GetDirectBufferAddress(byteBuf);

    jbyteArray byteArray = NULL;

    ScopedLocalRef<jclass> byteBufClass(env, env->FindClass("java/nio/ByteBuffer"));
    CHECK(byteBufClass.get() != NULL);

    if (ptr == NULL) {
        jmethodID arrayID =
            env->GetMethodID(byteBufClass.get(), "array", "()[B");
        CHECK(arrayID != NULL);

        byteArray =
            (jbyteArray)env->CallObjectMethod(byteBuf, arrayID);

        if (byteArray == NULL) {
            return INVALID_OPERATION;
        }

        jboolean isCopy;
        ptr = env->GetByteArrayElements(byteArray, &isCopy);
    }

    if ((jint)length + offset > limit) {
        if (byteArray != NULL) {
            env->ReleaseByteArrayElements(byteArray, (jbyte *)ptr, 0);
        }

        return -ERANGE;
    }

    *outPtr = ptr;
    *outByteArray = byteArray;

    return OK;
}

JDescrambler::JDescrambler(JNIEnv *env, jobject descramblerBinderObj) {
    mDescrambler = GetDescrambler(env, descramblerBinderObj);
    if (mDescrambler == NULL) {
        jniThrowException(env, "java/lang/NullPointerException", NULL);
    }
}

JDescrambler::~JDescrambler() {
    // Don't call release() here, it's called by Java class
    mDescrambler.clear();
    mMem.clear();
    mDealer.clear();
}

// static
sp<IDescrambler> JDescrambler::GetDescrambler(JNIEnv *env, jobject obj) {
    if (obj != NULL) {
        sp<hardware::IBinder> hwBinder =
                JHwRemoteBinder::GetNativeContext(env, obj)->getBinder();

        if (hwBinder != NULL) {
            return hardware::fromBinder<
                    IDescrambler, BpHwDescrambler, BnHwDescrambler>(hwBinder);
        }
    }
    return NULL;
}

bool JDescrambler::ensureBufferCapacity(size_t neededSize) {
    if (mMem != NULL && mMem->size() >= neededSize) {
        return true;
    }

    ALOGV("ensureBufferCapacity: current size %zu, new size %zu",
            mMem == NULL ? 0 : mMem->size(), neededSize);

    size_t alignment = MemoryDealer::getAllocationAlignment();
    neededSize = (neededSize + (alignment - 1)) & ~(alignment - 1);
    // Align to multiples of 64K.
    neededSize = (neededSize + 65535) & ~65535;
    mDealer = new MemoryDealer(neededSize, "JDescrambler");
    mMem = mDealer->allocate(neededSize);

    ssize_t offset;
    size_t size;
    sp<IMemoryHeap> heap = mMem->getMemory(&offset, &size);
    if (heap == NULL) {
        return false;
    }

    native_handle_t* nativeHandle = native_handle_create(1, 0);
    if (!nativeHandle) {
        ALOGE("ensureBufferCapacity: failed to create native handle");
        return false;
    }
    nativeHandle->data[0] = heap->getHeapID();
    mDescramblerSrcBuffer.heapBase = hidl_memory("ashmem",
            hidl_handle(nativeHandle), heap->getSize());
    mDescramblerSrcBuffer.offset = (uint64_t) offset;
    mDescramblerSrcBuffer.size = (uint64_t) size;
    return true;
}

status_t JDescrambler::descramble(
        jbyte key,
        ssize_t totalLength,
        const hidl_vec<SubSample>& subSamples,
        const void *srcPtr,
        jint srcOffset,
        void *dstPtr,
        jint dstOffset,
        Status *status,
        uint32_t *bytesWritten,
        hidl_string *detailedError) {
    // TODO: IDescrambler::descramble() is re-entrant, however because we
    // only have 1 shared mem buffer, we can only do 1 descramble at a time.
    // Concurrency might be improved by allowing on-demand allocation of up
    // to 2 shared mem buffers.
    Mutex::Autolock autolock(mSharedMemLock);

    if (!ensureBufferCapacity(totalLength)) {
        return NO_MEMORY;
    }

    memcpy(mMem->pointer(),
            (const void*)((const uint8_t*)srcPtr + srcOffset), totalLength);

    DestinationBuffer dstBuffer;
    dstBuffer.type = BufferType::SHARED_MEMORY;
    dstBuffer.nonsecureMemory = mDescramblerSrcBuffer;

    auto err = mDescrambler->descramble(
            (ScramblingControl) key,
            subSamples,
            mDescramblerSrcBuffer,
            0,
            dstBuffer,
            0,
            [&status, &bytesWritten, &detailedError] (
                    Status _status, uint32_t _bytesWritten,
                    const hidl_string& _detailedError) {
                *status = _status;
                *bytesWritten = _bytesWritten;
                *detailedError = _detailedError;
            });

    if (!err.isOk()) {
        return FAILED_TRANSACTION;
    }

    if (*status == Status::OK) {
        if (*bytesWritten > 0 && (ssize_t) *bytesWritten <= totalLength) {
            memcpy((void*)((uint8_t*)dstPtr + dstOffset), mMem->pointer(), *bytesWritten);
        } else {
            // status seems OK but bytesWritten is invalid, we really
            // have no idea what is wrong.
            *status = Status::ERROR_CAS_UNKNOWN;
        }
    }
    return OK;
}

}  // namespace android

using namespace android;

static void android_media_MediaDescrambler_native_release(JNIEnv *env, jobject thiz) {
    setDescrambler(env, thiz, NULL);
}

static void android_media_MediaDescrambler_native_init(JNIEnv *env) {
    ScopedLocalRef<jclass> clazz(
            env, env->FindClass("android/media/MediaDescrambler"));
    CHECK(clazz.get() != NULL);

    gFields.context = env->GetFieldID(clazz.get(), "mNativeContext", "J");
    CHECK(gFields.context != NULL);
}

static void android_media_MediaDescrambler_native_setup(
        JNIEnv *env, jobject thiz, jobject descramblerBinderObj) {
    setDescrambler(env, thiz, new JDescrambler(env, descramblerBinderObj));
}

static ssize_t getSubSampleInfo(JNIEnv *env, jint numSubSamples,
        jintArray numBytesOfClearDataObj, jintArray numBytesOfEncryptedDataObj,
        hidl_vec<SubSample> *outSubSamples) {

    if (numSubSamples <= 0 ||
            numSubSamples >= (signed)(INT32_MAX / sizeof(SubSample))) {
        // subSamples array may silently overflow if number of samples are
        // too large.  Use INT32_MAX as maximum allocation size may be less
        // than SIZE_MAX on some platforms.
        ALOGE("numSubSamples is invalid!");
        return -1;
    }

    jboolean isCopy;
    ssize_t totalSize = 0;

    jint *numBytesOfClearData =
        (numBytesOfClearDataObj == NULL)
            ? NULL
            : env->GetIntArrayElements(numBytesOfClearDataObj, &isCopy);

    jint *numBytesOfEncryptedData =
        (numBytesOfEncryptedDataObj == NULL)
            ? NULL
            : env->GetIntArrayElements(numBytesOfEncryptedDataObj, &isCopy);

    outSubSamples->resize(numSubSamples);
    SubSample *subSamples = outSubSamples->data();
    if (subSamples == NULL) {
        ALOGE("Failed to allocate SubSample array!");
        return -1;
    }

    for (jint i = 0; i < numSubSamples; ++i) {
        subSamples[i].numBytesOfClearData =
            (numBytesOfClearData == NULL) ? 0 : numBytesOfClearData[i];

        subSamples[i].numBytesOfEncryptedData =
            (numBytesOfEncryptedData == NULL)
                ? 0 : numBytesOfEncryptedData[i];

        totalSize += subSamples[i].numBytesOfClearData +
                subSamples[i].numBytesOfEncryptedData;
    }

    if (numBytesOfEncryptedData != NULL) {
        env->ReleaseIntArrayElements(
                numBytesOfEncryptedDataObj, numBytesOfEncryptedData, 0);
        numBytesOfEncryptedData = NULL;
    }

    if (numBytesOfClearData != NULL) {
        env->ReleaseIntArrayElements(
                numBytesOfClearDataObj, numBytesOfClearData, 0);
        numBytesOfClearData = NULL;
    }

    if (totalSize < 0) {
        return -1;
    }

    return totalSize;
}

static jthrowable createServiceSpecificException(
        JNIEnv *env, int serviceSpecificError, const char *msg) {
    if (env->ExceptionCheck()) {
        ALOGW("Discarding pending exception");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    ScopedLocalRef<jclass> clazz(
            env, env->FindClass("android/os/ServiceSpecificException"));
    CHECK(clazz.get() != NULL);

    const jmethodID ctor = env->GetMethodID(clazz.get(), "<init>", "(ILjava/lang/String;)V");
    CHECK(ctor != NULL);

    ScopedLocalRef<jstring> msgObj(
            env, env->NewStringUTF(msg != NULL ?
                    msg : String8::format("Error %#x", serviceSpecificError)));

    return (jthrowable)env->NewObject(
            clazz.get(), ctor, serviceSpecificError, msgObj.get());
}

static jint android_media_MediaDescrambler_native_descramble(
        JNIEnv *env, jobject thiz, jbyte key, jint numSubSamples,
        jintArray numBytesOfClearDataObj, jintArray numBytesOfEncryptedDataObj,
        jobject srcBuf, jint srcOffset, jint srcLimit,
        jobject dstBuf, jint dstOffset, jint dstLimit) {
    sp<JDescrambler> descrambler = getDescrambler(env, thiz);
    if (descrambler == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Invalid descrambler object!");
        return -1;
    }

    hidl_vec<SubSample> subSamples;
    ssize_t totalLength = getSubSampleInfo(
            env, numSubSamples, numBytesOfClearDataObj,
            numBytesOfEncryptedDataObj, &subSamples);
    if (totalLength < 0) {
        jniThrowException(env, "java/lang/IllegalArgumentException",
                "Invalid subsample info!");
        return -1;
    }

    void *srcPtr = NULL, *dstPtr = NULL;
    jbyteArray srcArray = NULL, dstArray = NULL;
    status_t err = getBufferAndSize(
            env, srcBuf, srcOffset, srcLimit, totalLength, &srcPtr, &srcArray);

    if (err == OK) {
        if (dstBuf == NULL) {
            dstPtr = srcPtr;
        } else {
            err = getBufferAndSize(
                    env, dstBuf, dstOffset, dstLimit, totalLength, &dstPtr, &dstArray);
        }
    }

    if (err != OK) {
        jniThrowException(env, "java/lang/IllegalArgumentException",
                "Invalid buffer offset and/or size for subsamples!");
        return -1;
    }

    Status status;
    uint32_t bytesWritten;
    hidl_string detailedError;

    err = descrambler->descramble(
            key, totalLength, subSamples,
            srcPtr, srcOffset, dstPtr, dstOffset,
            &status, &bytesWritten, &detailedError);

    // Release byte array before throwing
    if (srcArray != NULL) {
        env->ReleaseByteArrayElements(srcArray, (jbyte *)srcPtr, 0);
    }
    if (dstArray != NULL) {
        env->ReleaseByteArrayElements(dstArray, (jbyte *)dstPtr, 0);
    }

    if (err == NO_MEMORY) {
        jniThrowException(env, "java/lang/OutOfMemoryError", NULL);
    } else if (err == FAILED_TRANSACTION) {
        jniThrowException(env, "android/os/RemoteException", NULL);
    } else if (status != Status::OK) {
        // Throw ServiceSpecific with cas error code and detailed msg,
        // which will be re-thrown as MediaCasStateException.
        env->Throw(createServiceSpecificException(
                env, (int) status, detailedError.c_str()));
    }
    return bytesWritten;
}

static const JNINativeMethod gMethods[] = {
    { "native_release", "()V",
            (void *)android_media_MediaDescrambler_native_release },
    { "native_init", "()V",
            (void *)android_media_MediaDescrambler_native_init },
    { "native_setup", "(Landroid/os/IHwBinder;)V",
            (void *)android_media_MediaDescrambler_native_setup },
    { "native_descramble", "(BI[I[ILjava/nio/ByteBuffer;IILjava/nio/ByteBuffer;II)I",
            (void *)android_media_MediaDescrambler_native_descramble },
};

int register_android_media_Descrambler(JNIEnv *env) {
    return AndroidRuntime::registerNativeMethods(env,
                "android/media/MediaDescrambler", gMethods, NELEM(gMethods));
}

