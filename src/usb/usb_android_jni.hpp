// usb_android_jni.hpp - the one call android_main makes to put the Java USB
// layer (android/app/src/main/java/com/foxsdr/app/Usb.java) and this
// program's native USB transport in touch with each other.
//
// WHY THERE IS A SECOND FILE BESIDE usb_android_bridge.h. That header is the
// plain-C ABI - int, uint16_t, const char* - and stays free of <jni.h> on
// purpose (see its own comment on why one ABI seam at a time is the rule).
// This file is the JNI seam: it resolves the Java class, binds its five
// native methods to C++ functions, and calls Java back to say the binding is
// done. Everything it does with a registration it does BY CALLING
// foxsdr_usb_register()/foxsdr_usb_unregister(), so the plain-C contract
// stays the only way into the registry and nothing here needs to know the
// C++ types either.
//
// WHY THE METHODS ARE BOUND RATHER THAN NAMED. The ordinary way to reach
// native code from Java is System.loadLibrary() plus a
// Java_com_foxsdr_app_Usb_nativeRegister symbol, resolved by name. That
// route is not available here: libfoxsdr.so is loaded by the FRAMEWORK
// (NativeActivity dlopen()s the library named by android.app.lib_name in the
// manifest), not by System.loadLibrary from this application's class loader,
// so the runtime has no record of it to search for native symbols in and
// JNI_OnLoad is never called on it either. env->RegisterNatives() with
// explicit function pointers is what works regardless of how the library
// arrived, and it is the same mechanism Dear ImGui's own Android backend and
// every other NativeActivity application with a Java half uses.
//
// THE ORDERING PROBLEM THIS SOLVES, and it is the reason Java waits to be
// called rather than calling first. android_main runs on a thread the native
// glue spawns during the activity's onCreate; Java's own onCreate/onStart
// continue independently. A Java USB scan that ran first would call a native
// method that is not bound yet and get an UnsatisfiedLinkError with no radio
// and nothing in the log to explain it. So the flow is one-directional:
// android_main calls androidUsbInit(), which binds the methods and only then
// calls Usb.onNativeReady(activity) - after which Java owns the sequence
// (enumerate, ask permission, open, register) and native code is only ever
// called into.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::usb {

// Binds com.foxsdr.app.Usb's native methods and tells Java it may start. Both
// parameters are void* so this header stays free of <jni.h>, the same way
// core/net_post.hpp's androidNetInit() does: `javaVm` is the JavaVM*
// (android_app::activity->vm) and `activityObject` the jobject for the
// activity (android_app::activity->clazz).
//
// THE ACTIVITY IS NOT OPTIONAL, for the same reason the network transport's
// is not: a native thread resolves classes through the SYSTEM class loader,
// which cannot see com.foxsdr.app.Usb at all. The activity is what this asks
// for the application's own loader (getClassLoader().loadClass(...)).
// FindClass() alone would find nothing and there would be no error anywhere
// saying why.
//
// Safe to call once. Every step - the attach, the class lookup, the bind, the
// call into Java - is reported through core::diagLogf(), which on Android
// reaches logcat as well as the in-memory ring, because on a tablet that log
// is the whole account of why a plugged-in radio did or did not appear.
//
// A no-op on every non-Android build: there is no VM, no Java class and no
// UsbManager, and the desktop's own enumeration does not need one.
void androidUsbInit(void* javaVm, void* activityObject);

}  // namespace cascade::usb
