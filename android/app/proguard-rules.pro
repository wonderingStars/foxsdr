# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Kept even though minifyEnabled is false in both build types, because the rule
# below is the one that will be needed the moment it is turned on and it is
# cheaper to have it right now than to debug it later.
#
# A NativeActivity subclass is instantiated BY NAME, from the manifest, by the
# framework. R8 has no reference to it from any Kotlin or Java code - because
# there is none - so without this it is exactly the kind of class R8 removes as
# unreachable, and the app dies at launch with a ClassNotFoundException.
-keep class com.foxsdr.app.MainActivity { *; }

# The same reasoning for anything the native library ever calls back into by
# JNI signature.
-keepclasseswithmembernames class * {
    native <methods>;
}
